
#ifndef BLACKSMITH_SRC_UTILITIES_EXPERIMENTCONFIG_HPP
#define BLACKSMITH_SRC_UTILITIES_EXPERIMENTCONFIG_HPP

#include <string>
#include <unordered_map>

enum class execution_mode : int {
  BATCHED = 0,
  ALTERNATING = 1
};

static execution_mode get_exec_mode_from_string(const std::string &str) {
  std::unordered_map<std::string, execution_mode> lookup_map = {
      {"BATCHED", execution_mode::BATCHED},
      {"ALTERNATING", execution_mode::ALTERNATING},
  };
  return lookup_map[str];
}

static std::string get_string_from_execution_mode(const execution_mode &exm) {
  std::unordered_map<execution_mode, std::string> lookup_map = {
      {execution_mode::BATCHED, "BATCHED"},
      {execution_mode::ALTERNATING, "ALTERNATING"}
  };
  return lookup_map[exm];
}

class ExperimentConfig {
private:
  std::string filepath;

public:
  ExperimentConfig() = default;

  ExperimentConfig(const std::string &filepath, size_t config_id);

  ExperimentConfig(execution_mode ExecMode,
                   size_t NumMeasurementRounds,
                   size_t NumSyncRows,
                   size_t RowDistance,
                   bool RowOriginSameBg,
                   bool RowOriginSameBk);

  size_t config_id{};

  execution_mode exec_mode;

  size_t num_measurement_rounds{};

  size_t num_sync_rows{};

  size_t row_distance{};

  bool row_origin_same_bg{};

  bool row_origin_same_bk{};
};

#endif //BLACKSMITH_SRC_UTILITIES_EXPERIMENTCONFIG_HPP
