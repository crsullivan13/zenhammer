#include "Memory/ConflictCluster.hpp"
#include "Utilities/Logger.hpp"
#include "Utilities/CustomRandom.hpp"

#include <fstream>
#include <string>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <unordered_set>
#include <random>

std::string SimpleDramAddress::to_string_compact() const {
  char buff[1024];
  sprintf(buff, "(%2ld,%3ld,%2ld,%4ld,%p)",
          this->cluster_id, bg, bk, row_id, vaddr);
  return {buff};
}

std::string SimpleDramAddress::get_string_compact_desc() {
  return { "(cluster_id, bg, bk, row_id, vaddr)" };
}

ConflictCluster::ConflictCluster(std::string &filepath_rowlist,
                                 std::string &filepath_rowlist_bgbk) {
  cr = CustomRandom();
  load_bgbk_mapping(filepath_rowlist_bgbk);
  load_conflict_cluster(filepath_rowlist);
}

size_t ConflictCluster::get_typical_row_offset() {
  return typical_row_offset;
}

size_t ConflictCluster::get_min_num_rows() {
  return min_num_rows;
}

void ConflictCluster::load_conflict_cluster(const std::string &filename) {
  Logger::log_debug("Loading conflict cluster from '%s'", filename.c_str());

  std::unordered_map<uint64_t, size_t> offset_cnt;
  size_t total = 0;

  std::ifstream file(filename);
  if (!file.is_open())
    throw std::runtime_error("[-] could not open file " + filename);

  std::string last_bank_id;
  size_t row_id_cnt = 0;

  std::string bankid_vaddr_paddr;
  while (std::getline(file, bankid_vaddr_paddr, '\n')) {

    std::istringstream iss(bankid_vaddr_paddr);
    std::string item;
    std::vector<std::string> items;
    while (std::getline(iss, item, ',')) {
      items.push_back(item);
      item.clear();
    }

    auto cur_bank_id = items[0];
    auto cur_vaddr = items[1];
    auto cur_paddr = items[2];

    if (cur_bank_id != last_bank_id && !last_bank_id.empty())
      row_id_cnt = 0;

    SimpleDramAddress addr{};
    addr.cluster_id = (size_t) strtoll(cur_bank_id.c_str(), nullptr, 10);
    addr.row_id = row_id_cnt;
    addr.vaddr = (volatile char *) strtoull((const char *) cur_vaddr.c_str(), nullptr, 16);
    addr.paddr = (volatile char *) strtoull((const char *) cur_paddr.c_str(), nullptr, 16);

    if (!clusterid2bgbk.empty()) {
      // skip addresses where we
      if (clusterid2bgbk.find(addr.cluster_id) != clusterid2bgbk.end()) {
        auto bgbk = clusterid2bgbk[addr.cluster_id];
        addr.bg = bgbk.first;
        addr.bk = bgbk.second;
      } else {
        Logger::log_debug(format_string("skipping vaddr=%p as cluster_id=%d not in clusterid2bgbk",
                                        addr.vaddr, addr.cluster_id));
        continue;
      }
    }

    total++;
    clusters[addr.cluster_id][addr.row_id] = addr;

    // store a mapping from virtual address to SimpleDramAddress
    vaddr_map[addr.vaddr] = addr;

    if (last_bank_id == cur_bank_id && row_id_cnt > 0) {
      auto offt = (uint64_t)addr.vaddr-(uint64_t)clusters[addr.cluster_id][clusters[addr.cluster_id].size()-2].vaddr;
      offset_cnt[offt]++;
    }

#if (DEBUG==1)
    std::stringstream out;
    out << addr.cluster_id << " "
        << addr.row_id << " "
        << std::hex << "0x" << (uint64_t) addr.vaddr << " "
        << std::hex << "0x" << (uint64_t) addr.paddr
        << std::endl;
    Logger::log_debug(out.str());
#endif

    row_id_cnt++;
    last_bank_id = cur_bank_id;

    bankid_vaddr_paddr.clear();
  }

  // find the most common row offset
  using t = decltype(offset_cnt)::value_type;
  auto elem = std::max_element(offset_cnt.begin(), offset_cnt.end(),
                               [](const t &p1, const t &p2) { return p1.second < p2.second;
  });
  typical_row_offset = elem->second;

  // determine the minimum number of rows per bank (though all banks are supposed to have the same number of rows)
  min_num_rows = std::numeric_limits<std::size_t>::max();
  for (const auto &cluster : clusters) {
    min_num_rows = std::min(min_num_rows, cluster.second.size());
  }
}

SimpleDramAddress ConflictCluster::get_next_row(const SimpleDramAddress &addr) {
  return get_nth_next_row(addr, 1);
}

SimpleDramAddress ConflictCluster::get_nth_next_row(const SimpleDramAddress &addr, size_t nth) {
  auto next_row = (addr.row_id + nth) % clusters[addr.cluster_id].size();
  return clusters[addr.cluster_id][next_row];
}

SimpleDramAddress ConflictCluster::get_simple_dram_address(volatile char *vaddr) {
  if (vaddr_map.find(vaddr) != vaddr_map.end()) {
    return vaddr_map[vaddr];
  } else {
    uint64_t lowest_dist = std::numeric_limits<uint64_t>::max();
    uint64_t last_dist = std::numeric_limits<uint64_t>::max();
    auto it = vaddr_map.begin();
    SimpleDramAddress *addr = &vaddr_map.begin()->second;
    // we assume here that the vaddr_map is sorted
    while (last_dist <= lowest_dist && it != vaddr_map.end()) {
      auto dist = (uint64_t) it->first - (uint64_t) vaddr;
      if (dist < last_dist) {
        lowest_dist = dist;
        addr = &it->second;
      } else {
        break;
      }
      last_dist = dist;
      it++;
    }
    return *addr;
  }
}

void ConflictCluster::load_bgbk_mapping(const std::string &filepath) {
  std::unordered_set<std::string> all_bg_bk;

  Logger::log_debug("Loading cluster->(bg,bk) mapping from '%s'", filepath.c_str());

  std::ifstream file(filepath);
  if (!file.is_open())
    throw std::runtime_error("[-] could not open file " + filepath);

  std::string clusterid_bg_bk;
  while (std::getline(file, clusterid_bg_bk, '\n')) {
    std::istringstream iss(clusterid_bg_bk);
    std::string item;
    std::vector<std::string> items;
    while (std::getline(iss, item, ',')) {
      items.push_back(item);
      item.clear();
    }

    // skip line if it starts with '#' (e.g., header or comment)
    if (items[0].rfind('#', 0) == 0) {
      continue;
    }

    auto cluster_id = strtoul(items[0].c_str(), nullptr, 10);
    auto bg_id = strtoul(items[1].c_str(), nullptr, 2);
    auto bk_id = strtoul(items[2].c_str(), nullptr, 2);

    std::stringstream ss;
    ss << bg_id << "_" << bk_id;
    if (all_bg_bk.find(ss.str())!= all_bg_bk.end()) {
      throw std::runtime_error("[-] cluster->(bg,bk) mapping is not unique");
    } else {
      all_bg_bk.insert(ss.str());
    }
    clusterid2bgbk[cluster_id] = std::make_pair(bg_id, bk_id);
  }

  file.close();
}

SimpleDramAddress ConflictCluster::get_simple_dram_address(size_t bank_id, size_t row_id) {
  if (clusters.find(bank_id) == clusters.end())
    throw std::runtime_error("[-] invalid bank_id given!");
  return clusters[bank_id][row_id % clusters[bank_id].size()];
}

std::vector<size_t> ConflictCluster::get_supported_cluster_ids() {
  std::vector<size_t> supported_cluster_ids;
  supported_cluster_ids.reserve(clusters.size());
  for (const auto &entry : clusters)
    supported_cluster_ids.push_back(entry.first);
  return supported_cluster_ids;
}

std::vector<volatile char*> ConflictCluster::get_sync_rows(SimpleDramAddress &addr, size_t num_rows, bool verbose) {
  // build a list that alternates rows with <same bk, diff bg> and <diff bk, same bg> addresses relative to
  // the 'addr' passed to this function

  const size_t num_rows_per_subset = num_rows / 2;

  auto f_samebg_diffbk = [](size_t bg_target, size_t bg_candidate, size_t bk_target, size_t bk_candidate) {
    return (bg_target == bg_candidate) && (bk_target != bk_candidate);
  };
  auto samebg_diffbk = get_filtered_addresses(addr, num_rows_per_subset, verbose, f_samebg_diffbk);

  auto f_diffbg_samebk = [](size_t bg_target, size_t bg_candidate, size_t bk_target, size_t bk_candidate) {
    return (bg_target != bg_candidate) && (bk_target == bk_candidate);
  };
  auto diffbg_samebk = get_filtered_addresses(addr, num_rows_per_subset, verbose, f_diffbg_samebk);

  std::vector<volatile char*> sync_rows;
  sync_rows.reserve(num_rows);
  for (size_t i = 0; i < num_rows_per_subset; i++) {
    sync_rows.push_back(samebg_diffbk[i]);
    sync_rows.push_back(diffbg_samebk[i]);
  }

  return sync_rows;
}


std::vector<volatile char *> ConflictCluster::get_filtered_addresses(
    SimpleDramAddress &addr, size_t max_num_rows, bool verbose,
    bool (*func)(size_t bg_target, size_t bg_candidate, size_t bk_target, size_t bk_candidate)) {
  std::stringstream ss;
  for (const auto &cluster_id : clusterid2bgbk) {

    if (func(cluster_id.second.first, addr.bg, cluster_id.second.second, addr.bk)) {
//    if (cluster_id.second.first == addr.bg && cluster_id.second.second != addr.bk) {
      std::vector<volatile char*> result_cluster;
      for (const auto &a : clusters[cluster_id.first]) {
        result_cluster.push_back(a.second.vaddr);
        ss << a.second.to_string_compact() << "\n";
        if (result_cluster.size() == max_num_rows) {
          break;
        }
      }

      if (verbose) {
        Logger::log_info("Sync rows " + SimpleDramAddress::get_string_compact_desc() + " :");
        Logger::log_data(ss.str());
      }

      return result_cluster;
    }

  }

  Logger::log_error("get_samebg_diffbk_addresses could not find any <other bg, same bk> addresses!");
  exit(EXIT_FAILURE);
}

