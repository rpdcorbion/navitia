/* Copyright © 2001-2014, Canal TP and/or its affiliates. All rights reserved.
  
This file is part of Navitia,
    the software to build cool stuff with public transport.
 
Hope you'll enjoy and contribute to this project,
    powered by Canal TP (www.canaltp.fr).
Help us simplify mobility and open public transport:
    a non ending quest to the responsive locomotion way of traveling!
  
LICENCE: This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
   
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.
   
You should have received a copy of the GNU Affero General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
  
Stay tuned using
twitter @navitia 
IRC #navitia on freenode
https://groups.google.com/d/forum/navitia
www.navitia.io
*/

#include "traffic_reports_api.h"
#include "type/pb_converter.h"
#include "ptreferential/ptreferential.h"
#include "utils/logger.h"

namespace bt = boost::posix_time;

namespace navitia { namespace disruption {

namespace { // anonymous namespace

using DisruptionSet = std::set<boost::shared_ptr<type::disruption::Impact>, Less>;

struct NetworkDisrupt {
    type::idx_t idx;
    const type::Network* network = nullptr;
    DisruptionSet network_disruptions;
    //we use a vector of pair because we need to sort by the priority of the impacts
    std::vector<std::pair<const type::Line*, DisruptionSet>> lines;
    std::vector<std::pair<const type::StopArea*, DisruptionSet>> stop_areas;
    std::vector<std::pair<const type::VehicleJourney*, DisruptionSet>> vehicle_journeys;
};

class TrafficReport {
private:
    std::vector<NetworkDisrupt> disrupts;
    log4cplus::Logger logger;

    NetworkDisrupt& find_or_create(const type::Network* network);
    void add_stop_areas(const type::Indexes& network_idx,
                      const std::string& filter,
                      const std::vector<std::string>& forbidden_uris,
                      const type::Data &d,
                      const boost::posix_time::ptime now);

    void add_networks(const type::Indexes& network_idx,
                      const type::Data &d,
                      const boost::posix_time::ptime now);
    void add_lines(const std::string& filter,
                      const std::vector<std::string>& forbidden_uris,
                      const type::Data &d,
                      const boost::posix_time::ptime now);
    void add_vehicle_journeys(const type::Indexes& network_idx,
                              const std::string& filter,
                              const std::vector<std::string>& forbidden_uris,
                              const type::Data &d,
                              const boost::posix_time::ptime now);
    void sort_disruptions();
public:
    TrafficReport(): logger(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("logger"))) {}

    void disruptions_list(const std::string& filter,
                    const std::vector<std::string>& forbidden_uris,
                    const type::Data &d,
                    const boost::posix_time::ptime now);

    const std::vector<NetworkDisrupt>& get_disrupts() const {
        return this->disrupts;
    }

    size_t get_disrupts_size() {
        return this->disrupts.size();
    }
};

static int min_priority(const DisruptionSet& disruptions){
    int min = std::numeric_limits<int>::max();
    for(const auto& impact: disruptions){
        if(!impact->severity) continue;
        if(impact->severity->priority < min){
            min = impact->severity->priority;
        }
    }
    return min;
}


NetworkDisrupt& TrafficReport::find_or_create(const type::Network* network){
    auto find_predicate = [&](const NetworkDisrupt& network_disrupt) {
        return network == network_disrupt.network;
    };
    auto it = boost::find_if(this->disrupts, find_predicate);
    if(it == this->disrupts.end()){
        NetworkDisrupt dist;
        dist.network = network;
        dist.idx = this->disrupts.size();
        this->disrupts.push_back(std::move(dist));
        return disrupts.back();
    }
    return *it;
}

void TrafficReport::add_stop_areas(const type::Indexes& network_idx,
                      const std::string& filter,
                      const std::vector<std::string>& forbidden_uris,
                      const type::Data& d,
                      const boost::posix_time::ptime now){

    for (auto idx : network_idx) {
        const auto* network = d.pt_data->networks[idx];
        std::string new_filter = "network.uri=" + network->uri;
        if (!filter.empty()) {
            new_filter += " and " + filter;
        }
        type::Indexes stop_areas;

       try {
            stop_areas = ptref::make_query(type::Type_e::StopArea, new_filter, forbidden_uris, d);
        } catch (const ptref::parsing_error& parse_error) {
            LOG4CPLUS_WARN(logger, "Disruption::add_stop_areas : Unable to parse filter "
                                + parse_error.more);
        } catch (const ptref::ptref_error& /*ptref_error*/) {
           // that can arrive quite often if there is a filter, and
           // it's quite normal. Imagine /line/metro1/traffic_reports
           // for the network SNCF.
        }
        for (auto stop_area_idx: stop_areas) {
            const auto* stop_area = d.pt_data->stop_areas[stop_area_idx];
            auto v = stop_area->get_publishable_messages(now);
            for (const auto* stop_point: stop_area->stop_point_list) {
                auto vsp = stop_point->get_publishable_messages(now);
                v.insert(v.end(), vsp.begin(), vsp.end());
            }
            if (!v.empty()) {
                NetworkDisrupt& dist = this->find_or_create(network);
                auto find_predicate = [&](const std::pair<const type::StopArea*, DisruptionSet>& item) {
                    return item.first == stop_area;
                };
                auto it = boost::find_if(dist.stop_areas, find_predicate);
                if (it == dist.stop_areas.end()) {
                    dist.stop_areas.push_back(std::make_pair(stop_area, DisruptionSet(v.begin(), v.end())));
                } else {
                    it->second.insert(v.begin(), v.end());
                }
            }
        }
    }
}

void TrafficReport::add_vehicle_journeys(const type::Indexes& network_idx,
                                         const std::string& filter,
                                         const std::vector<std::string>& forbidden_uris,
                                         const type::Data& d,
                                         const boost::posix_time::ptime now){

    for (const auto idx : network_idx) {
        const auto* network = d.pt_data->networks[idx];
        std::string new_filter = "network.uri=" + network->uri + " and vehicle_journey.has_disruption()";
        if (!filter.empty()) {
            new_filter += " and " + filter;
        }
        type::Indexes vehicle_journeys;
        try {
            vehicle_journeys =
                    ptref::make_query(type::Type_e::VehicleJourney, new_filter, forbidden_uris, d);
        } catch (const ptref::parsing_error& parse_error) {
            LOG4CPLUS_WARN(logger, "Disruption::add_vehicle_journeys : Unable to parse filter "
                           << parse_error.more);
        } catch (const ptref::ptref_error&) {
        }
        for (const auto vj_idx: vehicle_journeys) {
            const auto* vj = d.pt_data->vehicle_journeys[vj_idx];
            auto impacts = vj->get_impacts();
            boost::remove_erase_if(impacts, [&](const boost::shared_ptr<type::disruption::Impact>& impact) {
                    if (! impact->disruption->is_publishable(now)) { return true; }
                    if (impact->severity->effect != type::disruption::Effect::NO_SERVICE) { return true; }
                    return false;
                });
            if (! impacts.empty()) {
                NetworkDisrupt& dist = this->find_or_create(network);
                auto find_predicate = [&](const std::pair<const type::VehicleJourney*, DisruptionSet>& item) {
                    return item.first == vj;
                };
                auto it = boost::find_if(dist.vehicle_journeys, find_predicate);
                if (it == dist.vehicle_journeys.end()) {
                    dist.vehicle_journeys.push_back({vj, DisruptionSet(impacts.begin(), impacts.end())});
                } else {
                    it->second.insert(impacts.begin(), impacts.end());
                }
            }
        }
    }
}

void TrafficReport::add_networks(const type::Indexes& network_idx,
                      const type::Data &d,
                      const boost::posix_time::ptime now){

    for(auto idx : network_idx){
        const auto* network = d.pt_data->networks[idx];
        if (network->has_publishable_message(now)){
            auto& res = this->find_or_create(network);
            auto v = network->get_publishable_messages(now);
            res.network_disruptions.insert(v.begin(), v.end());
        }
    }
}

void TrafficReport::add_lines(const std::string& filter,
                      const std::vector<std::string>& forbidden_uris,
                      const type::Data& d,
                      const boost::posix_time::ptime now){

    type::Indexes line_list;
    try {
        line_list  = ptref::make_query(type::Type_e::Line, filter, forbidden_uris, d);
    } catch(const ptref::parsing_error &parse_error) {
        LOG4CPLUS_WARN(logger, "Disruption::add_lines : Unable to parse filter " + parse_error.more);
    } catch(const ptref::ptref_error &ptref_error){
        LOG4CPLUS_WARN(logger, "Disruption::add_lines : ptref : "  + ptref_error.more);
    }
    for(auto idx : line_list){
        const auto* line = d.pt_data->lines[idx];
        auto v = line->get_publishable_messages(now);
        for(const auto* route: line->route_list){
            auto vr = route->get_publishable_messages(now);
            v.insert(v.end(), vr.begin(), vr.end());
        }
        if (!v.empty()){
            NetworkDisrupt& dist = this->find_or_create(line->network);
            auto find_predicate = [&](const std::pair<const type::Line*, DisruptionSet>& item) {
                return line == item.first;
            };
            auto it = boost::find_if(dist.lines, find_predicate);
            if(it == dist.lines.end()){
                dist.lines.push_back(std::make_pair(line, DisruptionSet(v.begin(), v.end())));
            }else{
                it->second.insert(v.begin(), v.end());
            }
        }
    }
}

void TrafficReport::sort_disruptions(){
    auto sort_disruption = [&](const NetworkDisrupt& d1, const NetworkDisrupt& d2){
            return d1.network->idx < d2.network->idx;
    };

    auto sort_lines = [&](const std::pair<const type::Line*, DisruptionSet>& l1,
                          const std::pair<const type::Line*, DisruptionSet>& l2) {
        int p1 = min_priority(l1.second);
        int p2 = min_priority(l2.second);
        if(p1 != p2){
            return p1 < p2;
        }else if(l1.first->code != l2.first->code){
            return l1.first->code < l2.first->code;
        }else{
            return l1.first->name < l2.first->name;
        }
    };

    std::sort(this->disrupts.begin(), this->disrupts.end(), sort_disruption);
    for(auto& disrupt : this->disrupts){
        std::sort(disrupt.lines.begin(), disrupt.lines.end(), sort_lines);
    }

}

void TrafficReport::disruptions_list(const std::string& filter,
                        const std::vector<std::string>& forbidden_uris,
                        const type::Data& d,
                        const boost::posix_time::ptime now){

    // if no disruptions, no need to make unnecessary treatment
    if (d.pt_data->disruption_holder.get_weak_impacts().empty()){
        return;
    }

    type::Indexes network_idx = ptref::make_query(type::Type_e::Network, filter,
                                                             forbidden_uris, d);
    add_networks(network_idx, d, now);
    add_lines(filter, forbidden_uris, d, now);
    add_stop_areas(network_idx, filter, forbidden_uris, d, now);
    add_vehicle_journeys(network_idx, filter, forbidden_uris, d, now);
    sort_disruptions();
}

} // anonymous namespace

pbnavitia::Response traffic_reports(const navitia::type::Data& d,
                                const boost::posix_time::ptime& current_datetime,
                                const size_t depth,
                                size_t count,
                                size_t start_page,
                                const std::string& filter,
                                const std::vector<std::string>& forbidden_uris) {

    PbCreator pb_creator(d, current_datetime, bt::time_period(current_datetime, bt::seconds(1)));

    TrafficReport result;
    try {
        result.disruptions_list(filter, forbidden_uris, d, current_datetime);
    } catch(const ptref::parsing_error& parse_error) {
        pb_creator.fill_pb_error(pbnavitia::Error::unable_to_parse, "Unable to parse filter" + parse_error.more);
        return pb_creator.get_response();
    } catch(const ptref::ptref_error& ptref_error) {
        pb_creator.fill_pb_error(pbnavitia::Error::bad_filter, "ptref : "  + ptref_error.more);
        return pb_creator.get_response();
    }

    size_t total_result = result.get_disrupts_size();
    std::vector<NetworkDisrupt> disrupts = paginate(result.get_disrupts(), count, start_page);
    for (const NetworkDisrupt& dist: disrupts) {
        auto* pb_traffic_reports = pb_creator.add_traffic_reports();
        pbnavitia::Network* pb_network = pb_traffic_reports->mutable_network();
        for(const auto& impact: dist.network_disruptions){
            pb_creator.fill_message(impact, pb_network, depth-1);
        }
        pb_creator.fill(dist.network, pb_network, depth, DumpMessage::No);
        for (const auto& line_item: dist.lines) {
            pbnavitia::Line* pb_line = pb_traffic_reports->add_lines();
            pb_creator.fill(line_item.first, pb_line, depth-1, DumpMessage::No);
            for(const auto& impact: line_item.second){
                pb_creator.fill_message(impact, pb_line, depth-1);
            }
        }
        for (const auto& sa_item: dist.stop_areas) {
            pbnavitia::StopArea* pb_stop_area = pb_traffic_reports->add_stop_areas();
            pb_creator.fill(sa_item.first, pb_stop_area, depth-1, DumpMessage::No);
            for(const auto& impact: sa_item.second){
                pb_creator.fill_message(impact, pb_stop_area, depth-1);
            }
        }
        for (const auto& vj_item: dist.vehicle_journeys) {
            pbnavitia::VehicleJourney* pb_vehicle_journey = pb_traffic_reports->add_vehicle_journeys();
            pb_creator.fill(vj_item.first, pb_vehicle_journey, depth-1, DumpMessage::No);
            for(const auto& impact: vj_item.second){
                pb_creator.fill_message(impact, pb_vehicle_journey, depth-1);
            }
        }
    }
    pb_creator.make_paginate(total_result, start_page, count, pb_creator.traffic_reports_size());
    if (pb_creator.traffic_reports_size() == 0) {
        pb_creator.fill_pb_error(pbnavitia::Error::no_solution, pbnavitia::NO_SOLUTION,
                                 "no solution found for this disruption");
    }
    return pb_creator.get_response();
}

}}//namespace navitia::disruption
