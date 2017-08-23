#include <fstream>

#include "vtr_log.h"
#include "vtr_assert.h"
#include "vtr_math.h"

#include "globals.h"
#include "timing_util.h"
#include "timing_info.h"

#include "read_sdc.h"

double sec_to_nanosec(double seconds) { return 1e9*seconds; }

double sec_to_mhz(double seconds) { return (1. / seconds) / 1e6; }

/*
 * Setup-time related
 */
tatum::TimingPathInfo find_longest_critical_path_delay(const tatum::TimingConstraints& constraints, const tatum::SetupTimingAnalyzer& setup_analyzer) {
    tatum::TimingPathInfo crit_path_info;

    auto& timing_ctx = g_vpr_ctx.timing();

    auto cpds = tatum::find_critical_paths(*timing_ctx.graph, constraints, setup_analyzer);

    //Record the maximum critical path accross all domain pairs
    for(const auto& path_info : cpds) {
        if(crit_path_info.delay() < path_info.delay() || std::isnan(crit_path_info.delay())) {
            crit_path_info = path_info;
        }
    }

    return crit_path_info;
}

tatum::TimingPathInfo find_least_slack_critical_path_delay(const tatum::TimingConstraints& constraints, const tatum::SetupTimingAnalyzer& setup_analyzer) {
    tatum::TimingPathInfo crit_path_info;

    auto& timing_ctx = g_vpr_ctx.timing();

    auto cpds = tatum::find_critical_paths(*timing_ctx.graph, constraints, setup_analyzer);

    //Record the maximum critical path accross all domain pairs
    for(const auto& path_info : cpds) {
        if(path_info.slack() < crit_path_info.slack() || std::isnan(crit_path_info.slack())) {
            crit_path_info = path_info;
        }
    }

    return crit_path_info;
}

float find_setup_total_negative_slack(const tatum::SetupTimingAnalyzer& setup_analyzer) {
    auto& timing_ctx = g_vpr_ctx.timing();

    float tns = 0.;
    for(tatum::NodeId node : timing_ctx.graph->logical_outputs()) {
        for(tatum::TimingTag tag : setup_analyzer.setup_slacks(node)) {
            float slack = tag.time().value();
            if(slack < 0.) {
                tns += slack;
            }
        }
    }
    return tns;
}

float find_setup_worst_negative_slack(const tatum::SetupTimingAnalyzer& setup_analyzer) {
    auto& timing_ctx = g_vpr_ctx.timing();

    float wns = 0.;
    for(tatum::NodeId node : timing_ctx.graph->logical_outputs()) {
        for(tatum::TimingTag tag : setup_analyzer.setup_slacks(node)) {
            float slack = tag.time().value();

            if(slack < 0.) {
                wns = std::min(wns, slack);
            }
        }
    }
    return wns;
}

float find_node_setup_slack(const tatum::SetupTimingAnalyzer& setup_analyzer, tatum::NodeId node, tatum::DomainId launch_domain, tatum::DomainId capture_domain) {
    for(const auto& tag : setup_analyzer.setup_slacks(node)) {
        if(tag.launch_clock_domain() == launch_domain &&
           tag.capture_clock_domain() == capture_domain) {
            return tag.time().value();
        }
    }

    return NAN;
}

std::vector<HistogramBucket> create_setup_slack_histogram(const tatum::SetupTimingAnalyzer& setup_analyzer, size_t num_bins) {
    auto& timing_ctx = g_vpr_ctx.timing();

    std::vector<HistogramBucket> histogram;

    //Find the min and max slacks
    float min_slack = std::numeric_limits<float>::infinity();
    float max_slack = -std::numeric_limits<float>::infinity();
    for(tatum::NodeId node : timing_ctx.graph->logical_outputs()) {
        for(tatum::TimingTag tag : setup_analyzer.setup_slacks(node)) {
            float slack = tag.time().value();
            
            min_slack = std::min(min_slack, slack);
            max_slack = std::max(max_slack, slack);
        }
    }

    //Determine the bin size
    float range = max_slack - min_slack;
    float bin_size = range / num_bins;

    //Create the buckets
    float bucket_min = min_slack;
    for(size_t ibucket = 0; ibucket < num_bins; ++ibucket) {
        float bucket_max = bucket_min + bin_size;

        histogram.emplace_back(bucket_min, bucket_max);

        bucket_min = bucket_max;
    }

    //To avoid round-off errors we force the max value of the last bucket equal to the max slack
    histogram[histogram.size()-1].max_value = max_slack;

    //Count the slacks into the buckets
    auto comp = [](const HistogramBucket& bucket, float slack) {
        return bucket.max_value < slack;
    };

    for(tatum::NodeId node : timing_ctx.graph->logical_outputs()) {
        for(tatum::TimingTag tag : setup_analyzer.setup_slacks(node)) {
            float slack = tag.time().value();
            
            //Find the bucket who's max is less than the current slack

            auto iter = std::lower_bound(histogram.begin(), histogram.end(), slack, comp);
            VTR_ASSERT(iter != histogram.end());
            
            iter->count++;
        }
    }

    return histogram;
}

void print_setup_timing_summary(const tatum::TimingConstraints& constraints, const tatum::SetupTimingAnalyzer& setup_analyzer) {
    auto& timing_ctx = g_vpr_ctx.timing();

    auto crit_paths = tatum::find_critical_paths(*timing_ctx.graph, constraints, setup_analyzer);

    auto least_slack_cpd = find_least_slack_critical_path_delay(constraints, setup_analyzer);
    vtr::printf("Final critical path: %g ns", sec_to_nanosec(least_slack_cpd.delay()));

    if(crit_paths.size() == 1) {
        //Fmax is only meaningful for a single-clock circuit
        vtr::printf(", Fmax: %g MHz", sec_to_mhz(least_slack_cpd.delay()));
    }
    vtr::printf("\n");

    vtr::printf("Setup Worst Negative Slack (sWNS): %g ns\n", sec_to_nanosec(find_setup_worst_negative_slack(setup_analyzer)));
    vtr::printf("Setup Total Negative Slack (sTNS): %g ns\n", sec_to_nanosec(find_setup_total_negative_slack(setup_analyzer)));
    vtr::printf("\n");

    vtr::printf_info("Setup slack histogram:\n");
    print_histogram(create_setup_slack_histogram(setup_analyzer));


    if (crit_paths.size() > 1) {
        //Multi-clock
        vtr::printf("\n");

        //Periods per constraint
		vtr::printf_info("Intra-domain critical path delays (CPDs):\n");
        for(const auto& path : crit_paths) {
            if(path.launch_domain() == path.capture_domain()) {
                vtr::printf("  %s to %s CPD: %g ns (%g MHz)\n",
                            constraints.clock_domain_name(path.launch_domain()).c_str(),
                            constraints.clock_domain_name(path.capture_domain()).c_str(),
                            sec_to_nanosec(path.delay()),
                            sec_to_mhz(path.delay()));
            }
        }
        vtr::printf("\n");

		vtr::printf_info("Inter-domain critical path delays (CPDs):\n");
        for(const auto& path : crit_paths) {
            if(path.launch_domain() != path.capture_domain()) {
                vtr::printf("  %s to %s CPD: %g ns (%g MHz)\n",
                            constraints.clock_domain_name(path.launch_domain()).c_str(),
                            constraints.clock_domain_name(path.capture_domain()).c_str(),
                            sec_to_nanosec(path.delay()),
                            sec_to_mhz(path.delay()));
            }
        }
        vtr::printf("\n");

        //Slack per constraint
		vtr::printf_info("Intra-domain worst setup slacks per constraint:\n");
        for(const auto& path : crit_paths) {
            if(path.launch_domain() == path.capture_domain()) {
                vtr::printf("  %s to %s worst setup slack: %g ns\n",
                            constraints.clock_domain_name(path.launch_domain()).c_str(),
                            constraints.clock_domain_name(path.capture_domain()).c_str(),
                            sec_to_nanosec(path.slack()));
            }
        }
        vtr::printf("\n");

		vtr::printf_info("Inter-domain worst setup slacks per constraint:\n");
        for(const auto& path : crit_paths) {
            if(path.launch_domain() != path.capture_domain()) {
                vtr::printf("  %s to %s worst setup slack: %g ns\n",
                            constraints.clock_domain_name(path.launch_domain()).c_str(),
                            constraints.clock_domain_name(path.capture_domain()).c_str(),
                            sec_to_nanosec(path.slack()));
            }
        }
    }

    //Calculate the intra-domain (i.e. same launch and capture domain) non-virtual geomean, and fanout-weighted periods
    if(crit_paths.size() > 1) {
        std::vector<double> intra_domain_cpds;
        std::vector<double> fanout_weighted_intra_domain_cpds;
        double total_intra_domain_fanout = 0.;
        auto clock_fanouts = count_clock_fanouts(*timing_ctx.graph, setup_analyzer);
        for(const auto& path : crit_paths) {
            if(path.launch_domain() == path.capture_domain() && !constraints.is_virtual_clock(path.launch_domain())) {
                intra_domain_cpds.push_back(path.delay());

                auto iter = clock_fanouts.find(path.launch_domain());
                VTR_ASSERT(iter != clock_fanouts.end());
                double fanout = iter->second;

                fanout_weighted_intra_domain_cpds.push_back(path.delay() * fanout);
                total_intra_domain_fanout += fanout;
            }
        }

        //Print multi-clock geomeans
        if(intra_domain_cpds.size() > 0) {
            vtr::printf("\n");
            double geomean_intra_domain_cpd = vtr::geomean(intra_domain_cpds.begin(), intra_domain_cpds.end());
            vtr::printf("Geometric mean non-virtual intra-domain period: %g ns (%g MHz)\n", 
                    sec_to_nanosec(geomean_intra_domain_cpd), 
                    sec_to_mhz(geomean_intra_domain_cpd));

            //Normalize weighted fanouts by total fanouts
            for(auto& weighted_cpd : fanout_weighted_intra_domain_cpds) {
                weighted_cpd /= total_intra_domain_fanout;
            }
            double fanout_weighted_geomean_intra_domain_cpd = vtr::geomean(fanout_weighted_intra_domain_cpds.begin(),
                                                                           fanout_weighted_intra_domain_cpds.end());
            vtr::printf("Fanout-weighted geomean non-virtual intra-domain period: %g ns (%g MHz)\n", 
                    sec_to_nanosec(fanout_weighted_geomean_intra_domain_cpd), 
                    sec_to_mhz(fanout_weighted_geomean_intra_domain_cpd));
        }
    }
    vtr::printf("\n");
}

/*
 * Hold-time related statistics
 */
float find_hold_total_negative_slack(const tatum::HoldTimingAnalyzer& hold_analyzer) {
    auto& timing_ctx = g_vpr_ctx.timing();

    float tns = 0.;
    for(tatum::NodeId node : timing_ctx.graph->logical_outputs()) {
        for(tatum::TimingTag tag : hold_analyzer.hold_slacks(node)) {
            float slack = tag.time().value();
            if(slack < 0.) {
                tns += slack;
            }
        }
    }
    return tns;

}

float find_hold_worst_negative_slack(const tatum::HoldTimingAnalyzer& hold_analyzer) {
    auto& timing_ctx = g_vpr_ctx.timing();

    float wns = 0.;
    for(tatum::NodeId node : timing_ctx.graph->logical_outputs()) {
        for(tatum::TimingTag tag : hold_analyzer.hold_slacks(node)) {
            float slack = tag.time().value();

            if(slack < 0.) {
                wns = std::min(wns, slack);
            }
        }
    }
    return wns;
}

float find_hold_worst_slack(const tatum::HoldTimingAnalyzer& hold_analyzer, const tatum::DomainId launch, const tatum::DomainId capture) {
    auto& timing_ctx = g_vpr_ctx.timing();

    float worst_slack = std::numeric_limits<float>::infinity();
    for(tatum::NodeId node : timing_ctx.graph->logical_outputs()) {
        for(tatum::TimingTag tag : hold_analyzer.hold_slacks(node)) {
            if (tag.launch_clock_domain() == launch && tag.capture_clock_domain() == capture) {
                float slack = tag.time().value();
                
                worst_slack = std::min(worst_slack, slack);
            }
        }
    }
    return worst_slack;
}

std::vector<HistogramBucket> create_hold_slack_histogram(const tatum::HoldTimingAnalyzer& hold_analyzer, size_t num_bins) {
    auto& timing_ctx = g_vpr_ctx.timing();

    std::vector<HistogramBucket> histogram;

    //Find the min and max slacks
    float min_slack = std::numeric_limits<float>::infinity();
    float max_slack = -std::numeric_limits<float>::infinity();
    for(tatum::NodeId node : timing_ctx.graph->logical_outputs()) {
        for(tatum::TimingTag tag : hold_analyzer.hold_slacks(node)) {
            float slack = tag.time().value();
            
            min_slack = std::min(min_slack, slack);
            max_slack = std::max(max_slack, slack);
        }
    }

    //Determine the bin size
    float range = max_slack - min_slack;
    float bin_size = range / num_bins;

    //Create the buckets
    float bucket_min = min_slack;
    for(size_t ibucket = 0; ibucket < num_bins; ++ibucket) {
        float bucket_max = bucket_min + bin_size;

        histogram.emplace_back(bucket_min, bucket_max);

        bucket_min = bucket_max;
    }

    //To avoid round-off errors we force the max value of the last bucket equal to the max slack
    histogram[histogram.size()-1].max_value = max_slack;

    //Count the slacks into the buckets
    auto comp = [](const HistogramBucket& bucket, float slack) {
        return bucket.max_value < slack;
    };

    for(tatum::NodeId node : timing_ctx.graph->logical_outputs()) {
        for(tatum::TimingTag tag : hold_analyzer.hold_slacks(node)) {
            float slack = tag.time().value();
            
            //Find the bucket who's max is less than the current slack
            auto iter = std::lower_bound(histogram.begin(), histogram.end(), slack, comp);
            VTR_ASSERT(iter != histogram.end());
            
            //Add to bucket
            iter->count++;
        }
    }

    return histogram;
}

void print_hold_timing_summary(const tatum::TimingConstraints& constraints, const tatum::HoldTimingAnalyzer& hold_analyzer) {
    vtr::printf("Hold Worst Negative Slack (hWNS): %g ns\n", sec_to_nanosec(find_hold_worst_negative_slack(hold_analyzer)));
    vtr::printf("Hold Total Negative Slack (hTNS): %g ns\n", sec_to_nanosec(find_hold_total_negative_slack(hold_analyzer)));
    vtr::printf("\n");

    vtr::printf_info("Hold slack histogram:\n");
    print_histogram(create_hold_slack_histogram(hold_analyzer));

    if (constraints.clock_domains().size() > 1) {
        //Multi-clock
        vtr::printf("\n");

        //Slack per constraint
		vtr::printf_info("Intra-domain worst hold slacks per constraint:\n");
        for(const auto& domain : constraints.clock_domains()) {
            float worst_slack = find_hold_worst_slack(hold_analyzer, domain, domain);

            if (worst_slack == std::numeric_limits<float>::infinity()) continue; //No path


            vtr::printf("  %s to %s worst hold slack: %g ns\n",
                        constraints.clock_domain_name(domain).c_str(),
                        constraints.clock_domain_name(domain).c_str(),
                        sec_to_nanosec(worst_slack));
        }
        vtr::printf("\n");

		vtr::printf_info("Inter-domain worst hold slacks per constraint:\n");
        for(const auto& launch_domain : constraints.clock_domains()) {
            for(const auto& capture_domain : constraints.clock_domains()) {
                if (launch_domain != capture_domain) {
                    float worst_slack = find_hold_worst_slack(hold_analyzer, launch_domain, capture_domain);

                    if (worst_slack == std::numeric_limits<float>::infinity()) continue; //No path

                    vtr::printf("  %s to %s worst hold slack: %g ns\n",
                                constraints.clock_domain_name(launch_domain).c_str(),
                                constraints.clock_domain_name(capture_domain).c_str(),
                                sec_to_nanosec(worst_slack));
                }
            }
        }
    }
    vtr::printf("\n");
}

/*
 * General utilities
 */
std::map<tatum::DomainId,size_t> count_clock_fanouts(const tatum::TimingGraph& timing_graph, const tatum::SetupTimingAnalyzer& setup_analyzer) {
    std::map<tatum::DomainId,size_t> fanouts;
    for(tatum::NodeId node : timing_graph.nodes()) {

        tatum::NodeType type = timing_graph.node_type(node);
        if(type == tatum::NodeType::SOURCE || type == tatum::NodeType::SINK) {
            for(auto tag : setup_analyzer.setup_tags(node, tatum::TagType::DATA_ARRIVAL)) {
                fanouts[tag.launch_clock_domain()] += 1;
            }
            for(auto tag : setup_analyzer.setup_tags(node, tatum::TagType::DATA_REQUIRED)) {
                fanouts[tag.launch_clock_domain()] += 1;
            }
        }
    }

    return fanouts;
}

/*
 * Slack and criticality calculation utilities
 */

//Return the criticality of a net's pin in the CLB netlist
float calculate_clb_net_pin_criticality(const SetupTimingInfo& timing_info, const IntraLbPbPinLookup& pb_gpin_lookup, int inet, int ipin) {
    auto& cluster_ctx = g_vpr_ctx.clustering();

    const t_net_pin& net_pin = cluster_ctx.clbs_nlist.net[inet].pins[ipin];

    //There may be multiple atom netlist pins connected to this CLB pin
    std::vector<AtomPinId> atom_pins = find_clb_pin_connected_atom_pins(net_pin.block, net_pin.block_pin, pb_gpin_lookup);

    //Take the maximum of the atom pin criticality as the CLB pin criticality
    float clb_pin_crit = 0.;
    for(const AtomPinId atom_pin : atom_pins) {
        clb_pin_crit = std::max(clb_pin_crit, timing_info.setup_pin_criticality(atom_pin));
    }

    return clb_pin_crit;
}

//Returns the worst (maximum) criticality of the set of slack tags specified. Requires the maximum
//required time and worst slack for all domain pairs represent by the slack tags
//
// Criticality (in [0., 1.]) represents how timing-critical something is, 
// 0. is non-critical and 1. is most-critical.
//
// This returns 'relaxed per constraint' criticaly as defined in:
//
//     M. Wainberg and V. Betz, "Robust Optimization of Multiple Timing Constraints," 
//         IEEE CAD, vol. 34, no. 12, pp. 1942-1953, Dec. 2015. doi: 10.1109/TCAD.2015.2440316
//
// which handles the trade-off between different timing constraints in multi-clock circuits.
//
// Note that unlike in Wainberg, we calculate the relaxed criticality as a post-processing step.
float calc_relaxed_criticality(const std::map<DomainPair,float>& domains_max_req,
                               const std::map<DomainPair,float>& domains_worst_slack,
                               const tatum::TimingTags::tag_range tags) {
    //Allowable round-off tolerance during criticality calculation
    constexpr float CRITICALITY_ROUND_OFF_TOLERANCE = 1e-4;


    //Record the maximum criticality over all the tags
    float max_crit = 0.;
    for(const auto& tag : tags) {

        VTR_ASSERT_MSG(tag.type() == tatum::TagType::SLACK, "Tags must be slacks to calculate criticality");

        float slack = tag.time().value();

        auto domain_pair = DomainPair(tag.launch_clock_domain(), tag.capture_clock_domain());

        auto iter = domains_max_req.find(domain_pair);
        VTR_ASSERT_MSG(iter != domains_max_req.end(), "Require the maximum required time for clock domain pair");
        float max_req = iter->second;


        iter = domains_worst_slack.find(domain_pair);
        VTR_ASSERT_MSG(iter != domains_worst_slack.end(), "Require the worst slack for clock domain pair");
        float worst_slack = iter->second;

        if(worst_slack < 0.) {
            //We shift slacks and required time by the most negative slack 
            //**in the domain**, to ensure criticality is bounded within [0., 1.]
            //
            //This corresponds to the 'relaxed' criticality from Wainberg et. al.
            float shift = -worst_slack;
            VTR_ASSERT(shift > 0.);

            slack += shift;
            max_req += shift;
        }
        VTR_ASSERT(max_req > 0.);

        float crit = 1. - (slack / max_req);

        //Soft check for reasonable criticality values
        VTR_ASSERT_MSG(crit >= 0. - CRITICALITY_ROUND_OFF_TOLERANCE, "Criticality should never be negative");
        VTR_ASSERT_MSG(crit <= 1. + CRITICALITY_ROUND_OFF_TOLERANCE, "Criticality should never be greather than one");

        //Clamp criticality to [0., 1.] to correct round-off
        crit = std::max(0.f, crit);
        crit = std::min(1.f, crit);

        max_crit = std::max(max_crit, crit);
    }
    VTR_ASSERT_MSG(max_crit >= 0., "Criticality should never be negative");
    VTR_ASSERT_MSG(max_crit <= 1., "Criticality should never be greather than one");

    return max_crit;
}

void print_tatum_cpds(std::vector<tatum::TimingPathInfo> cpds) {
    for(auto path : cpds) {
        vtr::printf("Tatum   %zu -> %zu: least_slack=%g cpd=%g\n", size_t(path.launch_domain()), size_t(path.capture_domain()), float(path.slack()), float(path.delay()));
    }
}

void compare_tatum_classic_constraints() {
    auto& timing_ctx = g_vpr_ctx.timing();

    if(timing_ctx.sdc) {
        vtr::printf("Comparing timing constraints:\n");
        for(int launch_clk = 0; launch_clk < timing_ctx.sdc->num_constrained_clocks; ++launch_clk) {
            tatum::DomainId launch_domain = timing_ctx.constraints->find_clock_domain(timing_ctx.sdc->constrained_clocks[launch_clk].name);
            VTR_ASSERT(launch_domain);

            for(int capture_clk = 0; capture_clk < timing_ctx.sdc->num_constrained_clocks; ++capture_clk) {
                tatum::DomainId capture_domain = timing_ctx.constraints->find_clock_domain(timing_ctx.sdc->constrained_clocks[capture_clk].name);
                VTR_ASSERT(capture_domain);
                
                tatum::Time constraint = timing_ctx.constraints->setup_constraint(launch_domain, capture_domain);

                vtr::printf("  %s -> %s Classic: %g Tatum: %g\n", 
                            timing_ctx.sdc->constrained_clocks[launch_clk].name, timing_ctx.sdc->constrained_clocks[capture_clk].name,
                            timing_ctx.sdc->domain_constraint[launch_clk][capture_clk],
                            constraint.value());
            }
        }
    }
}
