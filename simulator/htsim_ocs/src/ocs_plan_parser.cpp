#include "ocs_plan_parser.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "checked_arithmetic.h"
#include "generated_contract.h"
#include "nlohmann/json.hpp"
#include "ocs_validation_error.h"
#include "sha256.h"

namespace htsim_ocs {
namespace {

using Json = nlohmann::json;
constexpr std::uint64_t kMissingId =
    std::numeric_limits<std::uint64_t>::max();

[[noreturn]] void fail(const std::string& code, const std::string& pointer,
                       const std::string& message,
                       ExitCode exit_code = ExitCode::kInvalidOrUnsupported) {
    throw OcsValidationError(code, pointer, message, exit_code);
}

[[noreturn]] void schema_fail(const std::string& pointer,
                              const std::string& message) {
    fail("schema_validation_error", pointer, message);
}

std::string child_pointer(const std::string& parent, std::string_view child) {
    std::string escaped;
    escaped.reserve(child.size());
    for (const char value : child) {
        if (value == '~') {
            escaped += "~0";
        } else if (value == '/') {
            escaped += "~1";
        } else {
            escaped.push_back(value);
        }
    }
    return parent.empty() ? "/" + escaped : parent + "/" + escaped;
}

bool valid_utf8(std::string_view raw) {
    std::size_t index = 0;
    while (index < raw.size()) {
        const auto first = static_cast<unsigned char>(raw[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xe0U) == 0xc0U) {
            length = 2;
            code_point = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            length = 3;
            code_point = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            length = 4;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (index + length > raw.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(raw[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        if (code_point < minimum || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += length;
    }
    return true;
}

void reject_forbidden_number_tokens(std::string_view raw) {
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = 0; index < raw.size();) {
        const char value = raw[index];
        if (in_string) {
            ++index;
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                in_string = false;
            }
            continue;
        }
        if (value == '"') {
            in_string = true;
            ++index;
            continue;
        }
        const std::string_view remaining = raw.substr(index);
        if (remaining.rfind("NaN", 0) == 0 ||
            remaining.rfind("Infinity", 0) == 0 ||
            remaining.rfind("-Infinity", 0) == 0) {
            fail("non_finite_number", "/",
                 "non-finite wire number is forbidden");
        }
        if (value != '-' && (value < '0' || value > '9')) {
            ++index;
            continue;
        }
        const std::size_t start = index;
        while (index < raw.size()) {
            const char token_value = raw[index];
            if (token_value == ',' || token_value == ']' ||
                token_value == '}' || token_value == ':' ||
                std::isspace(static_cast<unsigned char>(token_value))) {
                break;
            }
            ++index;
        }
        const std::string_view token = raw.substr(start, index - start);
        if (!token.empty() && token.front() == '-') {
            fail("uint64_out_of_range", "/",
                 "negative wire integer is forbidden");
        }
        if (token.find_first_of(".eE") != std::string_view::npos) {
            fail("non_integer_number", "/", "wire numbers must be integers");
        }
        std::uint64_t parsed = 0;
        for (const char digit : token) {
            if (digit < '0' || digit > '9') {
                break;
            }
            const std::uint64_t parsed_digit =
                static_cast<std::uint64_t>(digit - '0');
            if (parsed > (std::numeric_limits<std::uint64_t>::max() -
                          parsed_digit) /
                             10U) {
                fail("uint64_out_of_range", "/",
                     "wire integer exceeds uint64");
            }
            parsed = parsed * 10U + parsed_digit;
        }
    }
}

class WireScanner final {
  public:
    explicit WireScanner(std::string_view raw) : raw_(raw) {}

    void scan() {
        skip_whitespace();
        scan_value();
        skip_whitespace();
        if (index_ != raw_.size()) {
            fail("invalid_json", "/", "trailing JSON data is forbidden");
        }
    }

  private:
    void skip_whitespace() {
        while (index_ < raw_.size() &&
               (raw_[index_] == ' ' || raw_[index_] == '\t' ||
                raw_[index_] == '\r' || raw_[index_] == '\n')) {
            ++index_;
        }
    }

    void scan_value() {
        skip_whitespace();
        if (index_ >= raw_.size()) {
            fail("invalid_json", "/", "unexpected end of JSON input");
        }
        switch (raw_[index_]) {
            case '{':
                scan_object();
                return;
            case '[':
                scan_array();
                return;
            case '"':
                scan_string();
                return;
            case 't':
                index_ += 4;
                return;
            case 'f':
                index_ += 5;
                return;
            case 'n':
                index_ += 4;
                return;
            default:
                scan_number();
                return;
        }
    }

    std::string scan_string() {
        const std::size_t start = index_++;
        bool escaped = false;
        while (index_ < raw_.size()) {
            const char value = raw_[index_++];
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                const std::string encoded(raw_.substr(start, index_ - start));
                return Json::parse(encoded).get<std::string>();
            }
        }
        fail("invalid_json", "/", "unterminated JSON string");
    }

    void scan_object() {
        ++index_;
        skip_whitespace();
        std::set<std::string> keys;
        if (index_ < raw_.size() && raw_[index_] == '}') {
            ++index_;
            return;
        }
        while (true) {
            skip_whitespace();
            const std::string key = scan_string();
            if (!keys.insert(key).second) {
                fail("duplicate_object_key", "/",
                     "duplicate object key: " + key);
            }
            skip_whitespace();
            ++index_;  // Syntax was already accepted by nlohmann: skip ':'.
            scan_value();
            skip_whitespace();
            const char delimiter = raw_[index_++];
            if (delimiter == '}') {
                return;
            }
        }
    }

    void scan_array() {
        ++index_;
        skip_whitespace();
        if (index_ < raw_.size() && raw_[index_] == ']') {
            ++index_;
            return;
        }
        while (true) {
            scan_value();
            skip_whitespace();
            const char delimiter = raw_[index_++];
            if (delimiter == ']') {
                return;
            }
        }
    }

    void scan_number() {
        const std::size_t start = index_;
        while (index_ < raw_.size()) {
            const char value = raw_[index_];
            if (value == ',' || value == ']' || value == '}' ||
                std::isspace(static_cast<unsigned char>(value))) {
                break;
            }
            ++index_;
        }
        const std::string_view token = raw_.substr(start, index_ - start);
        if (!token.empty() && token.front() == '-') {
            fail("uint64_out_of_range", "/",
                 "negative wire integer is forbidden");
        }
        if (token.find_first_of(".eE") != std::string_view::npos) {
            fail("non_integer_number", "/", "wire numbers must be integers");
        }
        std::uint64_t value = 0;
        for (const char digit : token) {
            if (digit < '0' || digit > '9') {
                fail("invalid_json", "/", "invalid JSON number");
            }
            const std::uint64_t parsed_digit =
                static_cast<std::uint64_t>(digit - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() -
                         parsed_digit) /
                            10U) {
                fail("uint64_out_of_range", "/",
                     "wire integer exceeds uint64");
            }
            value = value * 10U + parsed_digit;
        }
    }

    std::string_view raw_;
    std::size_t index_ = 0;
};

void require_exact_object(const Json& value, const std::string& pointer,
                          std::initializer_list<std::string_view> fields) {
    if (!value.is_object()) {
        schema_fail(pointer.empty() ? "/" : pointer, "expected object");
    }
    std::set<std::string> expected;
    for (const std::string_view field : fields) {
        expected.emplace(field);
        if (!value.contains(std::string(field))) {
            schema_fail(child_pointer(pointer, field), "required field is missing");
        }
    }
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        if (expected.count(iterator.key()) == 0) {
            schema_fail(child_pointer(pointer, iterator.key()),
                        "unknown field is forbidden");
        }
    }
}

const Json& require_array(const Json& value, const std::string& pointer,
                          bool nonempty = false) {
    if (!value.is_array() || (nonempty && value.empty())) {
        schema_fail(pointer, nonempty ? "expected nonempty array"
                                      : "expected array");
    }
    return value;
}

std::uint64_t read_u64(const Json& value, const std::string& pointer,
                       bool positive = false,
                       std::uint64_t maximum =
                           std::numeric_limits<std::uint64_t>::max()) {
    if (!value.is_number_unsigned()) {
        schema_fail(pointer, "expected uint64 integer");
    }
    const std::uint64_t parsed = value.get<std::uint64_t>();
    if ((positive && parsed == 0) || parsed > maximum) {
        schema_fail(pointer, "integer is outside the allowed range");
    }
    return parsed;
}

std::optional<std::uint64_t> read_nullable_u64(
    const Json& value, const std::string& pointer) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return read_u64(value, pointer);
}

std::string read_string(const Json& value, const std::string& pointer,
                        bool nonempty = false) {
    if (!value.is_string()) {
        schema_fail(pointer, "expected string");
    }
    const std::string parsed = value.get<std::string>();
    if (nonempty && parsed.empty()) {
        schema_fail(pointer, "string must not be empty");
    }
    return parsed;
}

std::string read_enum(const Json& value, const std::string& pointer,
                      std::initializer_list<std::string_view> allowed) {
    const std::string parsed = read_string(value, pointer);
    for (const std::string_view candidate : allowed) {
        if (parsed == candidate) {
            return parsed;
        }
    }
    schema_fail(pointer, "string is not in the closed enum");
}

std::string read_const(const Json& value, const std::string& pointer,
                       std::string_view expected) {
    const std::string parsed = read_string(value, pointer);
    if (parsed != expected) {
        schema_fail(pointer, "constant string does not match schema");
    }
    return parsed;
}

bool is_lower_hex(const std::string& value, std::size_t length) {
    return value.size() == length &&
           std::all_of(value.begin(), value.end(), [](const char item) {
               return (item >= '0' && item <= '9') ||
                      (item >= 'a' && item <= 'f');
           });
}

std::optional<std::string> read_nullable_digest(const Json& value,
                                                const std::string& pointer,
                                                std::size_t length) {
    if (value.is_null()) {
        return std::nullopt;
    }
    const std::string parsed = read_string(value, pointer);
    if (!is_lower_hex(parsed, length)) {
        schema_fail(pointer, "digest has invalid lowercase hexadecimal form");
    }
    return parsed;
}

std::vector<std::uint64_t> read_u64_array(const Json& value,
                                          const std::string& pointer,
                                          bool nonempty,
                                          bool unique) {
    require_array(value, pointer, nonempty);
    std::vector<std::uint64_t> result;
    result.reserve(value.size());
    std::set<std::uint64_t> seen;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::uint64_t parsed =
            read_u64(value[index], child_pointer(pointer, std::to_string(index)));
        if (unique && !seen.insert(parsed).second) {
            schema_fail(pointer, "array values must be unique");
        }
        result.push_back(parsed);
    }
    return result;
}

OcsUnits parse_units(const Json& value, const std::string& pointer) {
    require_exact_object(value, pointer, {"data", "rate", "time"});
    return {read_const(value["data"], child_pointer(pointer, "data"), "byte"),
            read_const(value["rate"], child_pointer(pointer, "rate"),
                       "bit_per_second"),
            read_const(value["time"], child_pointer(pointer, "time"),
                       "picosecond")};
}

OcsWorkloadSpec parse_workload(const Json& value,
                               const std::string& pointer) {
    require_exact_object(value, pointer,
                         {"collective_id", "algorithm_id",
                          "algorithm_semantics_version",
                          "collective_semantics_version", "rank_count",
                          "message_bytes_per_rank"});
    return {
        read_string(value["collective_id"],
                    child_pointer(pointer, "collective_id"), true),
        read_string(value["algorithm_id"],
                    child_pointer(pointer, "algorithm_id"), true),
        read_string(value["algorithm_semantics_version"],
                    child_pointer(pointer, "algorithm_semantics_version"), true),
        read_string(value["collective_semantics_version"],
                    child_pointer(pointer, "collective_semantics_version"), true),
        read_u64(value["rank_count"], child_pointer(pointer, "rank_count"),
                 true),
        read_u64(value["message_bytes_per_rank"],
                 child_pointer(pointer, "message_bytes_per_rank")),
    };
}

OcsTopologySpec parse_topology(const Json& value,
                               const std::string& pointer) {
    require_exact_object(
        value, pointer,
        {"node_count", "plane_count", "per_plane_bps", "data_latency_ps",
         "reconfiguration_delay_ps", "duplex",
         "initial_configuration_policy"});
    return {
        read_u64(value["node_count"], child_pointer(pointer, "node_count"),
                 true),
        read_u64(value["plane_count"], child_pointer(pointer, "plane_count"),
                 true),
        read_u64(value["per_plane_bps"],
                 child_pointer(pointer, "per_plane_bps"), true),
        read_u64(value["data_latency_ps"],
                 child_pointer(pointer, "data_latency_ps")),
        read_u64(value["reconfiguration_delay_ps"],
                 child_pointer(pointer, "reconfiguration_delay_ps")),
        read_const(value["duplex"], child_pointer(pointer, "duplex"), "full"),
        read_const(value["initial_configuration_policy"],
                   child_pointer(pointer, "initial_configuration_policy"),
                   "first_use_preinstalled"),
    };
}

OcsTransportSpec parse_transport(const Json& value,
                                 const std::string& pointer) {
    require_exact_object(value, pointer,
                         {"mode", "wire_model", "packetization", "mtu_bytes",
                          "loss_mode", "ack_mode", "completion_semantics"});
    return {
        read_const(value["mode"], child_pointer(pointer, "mode"), "paper_exact"),
        read_const(value["wire_model"], child_pointer(pointer, "wire_model"),
                   "payload_only"),
        read_const(value["packetization"],
                   child_pointer(pointer, "packetization"), "exact_tail"),
        read_u64(value["mtu_bytes"], child_pointer(pointer, "mtu_bytes"), true,
                 contract::kMaxMtuBytes),
        read_const(value["loss_mode"], child_pointer(pointer, "loss_mode"),
                   "lossless"),
        read_const(value["ack_mode"], child_pointer(pointer, "ack_mode"), "none"),
        read_const(value["completion_semantics"],
                   child_pointer(pointer, "completion_semantics"),
                   "receiver_last_payload_byte"),
    };
}

OcsRunLimits parse_run_limits(const Json& value,
                              const std::string& pointer) {
    require_exact_object(value, pointer,
                         {"seed", "max_sim_time_ps", "max_events"});
    return {
        read_u64(value["seed"], child_pointer(pointer, "seed")),
        read_u64(value["max_sim_time_ps"],
                 child_pointer(pointer, "max_sim_time_ps"), true),
        read_u64(value["max_events"], child_pointer(pointer, "max_events"), true),
    };
}

std::vector<OcsConfiguration> parse_configurations(
    const Json& value, const std::string& pointer) {
    require_array(value, pointer);
    std::vector<OcsConfiguration> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::string item_pointer =
            child_pointer(pointer, std::to_string(index));
        require_exact_object(value[index], item_pointer,
                             {"configuration_id", "permutation"});
        result.push_back({
            read_u64(value[index]["configuration_id"],
                     child_pointer(item_pointer, "configuration_id")),
            read_u64_array(value[index]["permutation"],
                           child_pointer(item_pointer, "permutation"), true,
                           false),
        });
    }
    return result;
}

std::vector<OcsLogicalSegment> parse_segments(const Json& value,
                                              const std::string& pointer) {
    require_array(value, pointer);
    std::vector<OcsLogicalSegment> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::string item_pointer =
            child_pointer(pointer, std::to_string(index));
        require_exact_object(value[index], item_pointer,
                             {"segment_id", "buffer_offset_bytes",
                              "length_bytes", "origin_rank_ids",
                              "final_destination_rank_ids"});
        result.push_back({
            read_u64(value[index]["segment_id"],
                     child_pointer(item_pointer, "segment_id")),
            read_u64(value[index]["buffer_offset_bytes"],
                     child_pointer(item_pointer, "buffer_offset_bytes")),
            read_u64(value[index]["length_bytes"],
                     child_pointer(item_pointer, "length_bytes"), true),
            read_u64_array(value[index]["origin_rank_ids"],
                           child_pointer(item_pointer, "origin_rank_ids"), true,
                           true),
            read_u64_array(value[index]["final_destination_rank_ids"],
                           child_pointer(item_pointer,
                                         "final_destination_rank_ids"),
                           true, true),
        });
    }
    return result;
}

std::vector<OcsReadinessToken> parse_tokens(const Json& value,
                                            const std::string& pointer) {
    require_array(value, pointer, true);
    std::vector<OcsReadinessToken> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::string item_pointer =
            child_pointer(pointer, std::to_string(index));
        require_exact_object(value[index], item_pointer,
                             {"token_id", "token_type", "producer_id"});
        result.push_back({
            read_u64(value[index]["token_id"],
                     child_pointer(item_pointer, "token_id")),
            read_enum(value[index]["token_type"],
                      child_pointer(item_pointer, "token_type"),
                      {"collective_start", "flow_group_complete",
                       "step_complete"}),
            read_nullable_u64(value[index]["producer_id"],
                              child_pointer(item_pointer, "producer_id")),
        });
    }
    return result;
}

std::vector<OcsLogicalStepSpec> parse_steps(const Json& value,
                                            const std::string& pointer) {
    require_array(value, pointer);
    std::vector<OcsLogicalStepSpec> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::string item_pointer =
            child_pointer(pointer, std::to_string(index));
        require_exact_object(value[index], item_pointer,
                             {"step_id", "phase", "flow_group_ids",
                              "completion_token_id"});
        result.push_back({
            read_u64(value[index]["step_id"],
                     child_pointer(item_pointer, "step_id")),
            read_string(value[index]["phase"],
                        child_pointer(item_pointer, "phase"), true),
            read_u64_array(value[index]["flow_group_ids"],
                           child_pointer(item_pointer, "flow_group_ids"), true,
                           true),
            read_u64(value[index]["completion_token_id"],
                     child_pointer(item_pointer, "completion_token_id")),
        });
    }
    return result;
}

std::vector<OcsFlowGroupSpec> parse_groups(const Json& value,
                                           const std::string& pointer) {
    require_array(value, pointer);
    std::vector<OcsFlowGroupSpec> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::string item_pointer =
            child_pointer(pointer, std::to_string(index));
        require_exact_object(
            value[index], item_pointer,
            {"flow_group_id", "step_id", "plane_id", "program_epoch_id",
             "configuration_id", "depends_on_token_ids",
             "completion_token_id", "flow_ids"});
        result.push_back({
            read_u64(value[index]["flow_group_id"],
                     child_pointer(item_pointer, "flow_group_id")),
            read_u64(value[index]["step_id"],
                     child_pointer(item_pointer, "step_id")),
            read_u64(value[index]["plane_id"],
                     child_pointer(item_pointer, "plane_id")),
            read_u64(value[index]["program_epoch_id"],
                     child_pointer(item_pointer, "program_epoch_id")),
            read_u64(value[index]["configuration_id"],
                     child_pointer(item_pointer, "configuration_id")),
            read_u64_array(value[index]["depends_on_token_ids"],
                           child_pointer(item_pointer, "depends_on_token_ids"),
                           true, true),
            read_u64(value[index]["completion_token_id"],
                     child_pointer(item_pointer, "completion_token_id")),
            read_u64_array(value[index]["flow_ids"],
                           child_pointer(item_pointer, "flow_ids"), true, true),
        });
    }
    return result;
}

std::vector<OcsFlowSpec> parse_flows(const Json& value,
                                     const std::string& pointer) {
    require_array(value, pointer);
    std::vector<OcsFlowSpec> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::string item_pointer =
            child_pointer(pointer, std::to_string(index));
        require_exact_object(value[index], item_pointer,
                             {"flow_id", "flow_group_id", "src_rank",
                              "dst_rank", "payload_bytes", "segment_slices"});
        const Json& slices = require_array(
            value[index]["segment_slices"],
            child_pointer(item_pointer, "segment_slices"), true);
        std::vector<OcsSegmentSlice> parsed_slices;
        parsed_slices.reserve(slices.size());
        for (std::size_t slice_index = 0; slice_index < slices.size();
             ++slice_index) {
            const std::string slice_pointer = child_pointer(
                child_pointer(item_pointer, "segment_slices"),
                std::to_string(slice_index));
            require_exact_object(slices[slice_index], slice_pointer,
                                 {"segment_id", "segment_offset_bytes",
                                  "length_bytes"});
            parsed_slices.push_back({
                read_u64(slices[slice_index]["segment_id"],
                         child_pointer(slice_pointer, "segment_id")),
                read_u64(slices[slice_index]["segment_offset_bytes"],
                         child_pointer(slice_pointer, "segment_offset_bytes")),
                read_u64(slices[slice_index]["length_bytes"],
                         child_pointer(slice_pointer, "length_bytes"), true),
            });
        }
        result.push_back({
            read_u64(value[index]["flow_id"],
                     child_pointer(item_pointer, "flow_id")),
            read_u64(value[index]["flow_group_id"],
                     child_pointer(item_pointer, "flow_group_id")),
            read_u64(value[index]["src_rank"],
                     child_pointer(item_pointer, "src_rank")),
            read_u64(value[index]["dst_rank"],
                     child_pointer(item_pointer, "dst_rank")),
            read_u64(value[index]["payload_bytes"],
                     child_pointer(item_pointer, "payload_bytes"), true,
                     contract::kMaxPayloadBytes),
            std::move(parsed_slices),
        });
    }
    return result;
}

std::vector<OcsPlaneProgram> parse_programs(const Json& value,
                                            const std::string& pointer) {
    require_array(value, pointer);
    std::vector<OcsPlaneProgram> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::string item_pointer =
            child_pointer(pointer, std::to_string(index));
        require_exact_object(value[index], item_pointer, {"plane_id", "epochs"});
        const Json& epochs = require_array(value[index]["epochs"],
                                           child_pointer(item_pointer, "epochs"));
        std::vector<OcsProgramEpochSpec> parsed_epochs;
        parsed_epochs.reserve(epochs.size());
        for (std::size_t epoch_index = 0; epoch_index < epochs.size();
             ++epoch_index) {
            const std::string epoch_pointer = child_pointer(
                child_pointer(item_pointer, "epochs"),
                std::to_string(epoch_index));
            require_exact_object(
                epochs[epoch_index], epoch_pointer,
                {"program_epoch_id", "transition", "configuration_id",
                 "path_prep_not_before_token_ids", "flow_group_ids"});
            parsed_epochs.push_back({
                read_u64(epochs[epoch_index]["program_epoch_id"],
                         child_pointer(epoch_pointer, "program_epoch_id")),
                read_enum(epochs[epoch_index]["transition"],
                          child_pointer(epoch_pointer, "transition"),
                          {"initial", "reconfigure", "retain"}),
                read_u64(epochs[epoch_index]["configuration_id"],
                         child_pointer(epoch_pointer, "configuration_id")),
                read_u64_array(
                    epochs[epoch_index]["path_prep_not_before_token_ids"],
                    child_pointer(epoch_pointer,
                                  "path_prep_not_before_token_ids"),
                    false, true),
                read_u64_array(epochs[epoch_index]["flow_group_ids"],
                               child_pointer(epoch_pointer, "flow_group_ids"),
                               true, true),
            });
        }
        result.push_back({
            read_u64(value[index]["plane_id"],
                     child_pointer(item_pointer, "plane_id")),
            std::move(parsed_epochs),
        });
    }
    return result;
}

OcsPlannerCertificate parse_certificate(const Json& value,
                                        const std::string& pointer) {
    require_exact_object(
        value, pointer,
        {"planner_name", "planner_version", "solver_name", "solver_status",
         "objective_ps", "bound_ps", "relative_gap_ppm", "decision_sha256",
         "nominal_schedule", "integer_lowering_rule",
         "integer_lowering_version"});
    std::optional<std::string> solver_name;
    if (!value["solver_name"].is_null()) {
        solver_name = read_string(value["solver_name"],
                                  child_pointer(pointer, "solver_name"), true);
    }
    const Json& schedule = require_array(
        value["nominal_schedule"], child_pointer(pointer, "nominal_schedule"));
    std::vector<OcsNominalScheduleEntry> parsed_schedule;
    parsed_schedule.reserve(schedule.size());
    for (std::size_t index = 0; index < schedule.size(); ++index) {
        const std::string item_pointer = child_pointer(
            child_pointer(pointer, "nominal_schedule"), std::to_string(index));
        require_exact_object(schedule[index], item_pointer,
                             {"flow_group_id", "planned_release_ps",
                              "planned_complete_ps"});
        parsed_schedule.push_back({
            read_u64(schedule[index]["flow_group_id"],
                     child_pointer(item_pointer, "flow_group_id")),
            read_u64(schedule[index]["planned_release_ps"],
                     child_pointer(item_pointer, "planned_release_ps")),
            read_u64(schedule[index]["planned_complete_ps"],
                     child_pointer(item_pointer, "planned_complete_ps")),
        });
    }
    return {
        read_string(value["planner_name"], child_pointer(pointer, "planner_name"),
                    true),
        read_string(value["planner_version"],
                    child_pointer(pointer, "planner_version"), true),
        std::move(solver_name),
        read_enum(value["solver_status"], child_pointer(pointer, "solver_status"),
                  {"feasible", "not_used", "optimal",
                   "time_limit_with_incumbent"}),
        read_nullable_u64(value["objective_ps"],
                          child_pointer(pointer, "objective_ps")),
        read_nullable_u64(value["bound_ps"], child_pointer(pointer, "bound_ps")),
        read_nullable_u64(value["relative_gap_ppm"],
                          child_pointer(pointer, "relative_gap_ppm")),
        read_nullable_digest(value["decision_sha256"],
                             child_pointer(pointer, "decision_sha256"), 64),
        std::move(parsed_schedule),
        read_string(value["integer_lowering_rule"],
                    child_pointer(pointer, "integer_lowering_rule"), true),
        read_string(value["integer_lowering_version"],
                    child_pointer(pointer, "integer_lowering_version"), true),
    };
}

OcsProvenance parse_provenance(const Json& value,
                               const std::string& pointer) {
    require_exact_object(
        value, pointer,
        {"source_kind", "overlap4ocs_git_sha", "htsim_upstream_git_sha",
         "htsim_local_patchset_sha256", "instance_file_sha256",
         "program_file_sha256", "collective_ir_sha256",
         "plan_build_context_sha256"});
    return {
        read_enum(value["source_kind"], child_pointer(pointer, "source_kind"),
                  {"hand_authored_fixture", "production"}),
        read_nullable_digest(value["overlap4ocs_git_sha"],
                             child_pointer(pointer, "overlap4ocs_git_sha"), 40),
        read_nullable_digest(value["htsim_upstream_git_sha"],
                             child_pointer(pointer, "htsim_upstream_git_sha"),
                             40),
        read_nullable_digest(
            value["htsim_local_patchset_sha256"],
            child_pointer(pointer, "htsim_local_patchset_sha256"), 64),
        read_nullable_digest(value["instance_file_sha256"],
                             child_pointer(pointer, "instance_file_sha256"), 64),
        read_nullable_digest(value["program_file_sha256"],
                             child_pointer(pointer, "program_file_sha256"), 64),
        read_nullable_digest(value["collective_ir_sha256"],
                             child_pointer(pointer, "collective_ir_sha256"), 64),
        read_nullable_digest(
            value["plan_build_context_sha256"],
            child_pointer(pointer, "plan_build_context_sha256"), 64),
    };
}

OcsExecutionPlanV2 typed_plan(const Json& root) {
    require_exact_object(
        root, "",
        {"schema_version", "case_id", "strategy", "units", "workload",
         "topology", "transport", "execution_mode", "run_limits",
         "dependency_mode", "path_preparation_policy", "configurations",
         "logical_segments", "readiness_tokens", "steps", "flow_groups",
         "flows", "plane_programs", "planner_certificate", "provenance"});
    const std::string case_id = read_string(root["case_id"], "/case_id");
    if (case_id.size() != 69 || case_id.rfind("case-", 0) != 0 ||
        !is_lower_hex(case_id.substr(5), 64)) {
        schema_fail("/case_id", "case ID has invalid form");
    }
    return {
        read_const(root["schema_version"], "/schema_version",
                   contract::kPlanSchemaId),
        case_id,
        read_enum(root["strategy"], "/strategy",
                  {"baseline", "one_shot", "swot"}),
        parse_units(root["units"], "/units"),
        parse_workload(root["workload"], "/workload"),
        parse_topology(root["topology"], "/topology"),
        parse_transport(root["transport"], "/transport"),
        read_enum(root["execution_mode"], "/execution_mode",
                  {"exact_coalesced", "full_packet"}),
        parse_run_limits(root["run_limits"], "/run_limits"),
        read_enum(root["dependency_mode"], "/dependency_mode",
                  {"explicit_group_dag", "global_step_barrier"}),
        read_enum(root["path_preparation_policy"],
                  "/path_preparation_policy",
                  {"overlap_earliest", "static_preinstalled",
                   "step_lockstep"}),
        parse_configurations(root["configurations"], "/configurations"),
        parse_segments(root["logical_segments"], "/logical_segments"),
        parse_tokens(root["readiness_tokens"], "/readiness_tokens"),
        parse_steps(root["steps"], "/steps"),
        parse_groups(root["flow_groups"], "/flow_groups"),
        parse_flows(root["flows"], "/flows"),
        parse_programs(root["plane_programs"], "/plane_programs"),
        parse_certificate(root["planner_certificate"], "/planner_certificate"),
        parse_provenance(root["provenance"], "/provenance"),
        {},
    };
}

template <typename T, typename Getter>
void require_contiguous_ids(const std::vector<T>& values, Getter getter,
                            const std::string& pointer,
                            const std::string& field) {
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (getter(values[index]) != index) {
            fail("non_contiguous_id", pointer,
                 field + " values must be sorted and contiguous from zero");
        }
    }
}

void require_sorted_unique(const std::vector<std::uint64_t>& values,
                           const std::string& pointer) {
    if (!std::is_sorted(values.begin(), values.end()) ||
        std::adjacent_find(values.begin(), values.end()) != values.end()) {
        fail("noncanonical_id_array", pointer,
             "ID array must be sorted and unique");
    }
}

void require_index(std::uint64_t value, std::size_t size,
                   const std::string& pointer, const std::string& entity) {
    if (value >= size) {
        fail("reference_out_of_range", pointer,
             "reference names an unknown " + entity);
    }
}

void add_edge(std::map<std::string, std::set<std::string>>& edges,
              std::string source, std::string destination) {
    edges[std::move(source)].insert(std::move(destination));
}

void require_acyclic(
    const std::map<std::string, std::set<std::string>>& edges) {
    std::set<std::string> nodes;
    std::map<std::string, std::uint64_t> indegree;
    for (const auto& [source, destinations] : edges) {
        nodes.insert(source);
        for (const std::string& destination : destinations) {
            nodes.insert(destination);
            ++indegree[destination];
        }
    }
    std::set<std::string> ready;
    for (const std::string& node : nodes) {
        if (indegree[node] == 0) {
            ready.insert(node);
        }
    }
    std::size_t seen = 0;
    while (!ready.empty()) {
        const std::string node = *ready.begin();
        ready.erase(ready.begin());
        ++seen;
        const auto found = edges.find(node);
        if (found == edges.end()) {
            continue;
        }
        for (const std::string& destination : found->second) {
            if (--indegree[destination] == 0) {
                ready.insert(destination);
            }
        }
    }
    if (seen != nodes.size()) {
        fail("event_wait_graph_cycle", "/",
             "combined event wait-for graph has a cycle");
    }
}

class ResultBound final {
  public:
    explicit ResultBound(std::uint64_t limit) : limit_(limit) {}

    void add(std::uint64_t count, std::uint64_t bytes_each) {
        if (exceeded_) {
            return;
        }
        if (count != 0 && bytes_each > limit_ / count) {
            exceeded_ = true;
            return;
        }
        const std::uint64_t product = count * bytes_each;
        if (product > limit_ - value_) {
            exceeded_ = true;
            return;
        }
        value_ += product;
    }

    void add_product(std::uint64_t first, std::uint64_t second,
                     std::uint64_t bytes_each) {
        if (first != 0 && second > limit_ / first) {
            exceeded_ = true;
            return;
        }
        add(first * second, bytes_each);
    }

    bool exceeded() const { return exceeded_; }
    std::uint64_t value() const { return value_; }

  private:
    std::uint64_t limit_;
    std::uint64_t value_ = 0;
    bool exceeded_ = false;
};

std::uint64_t conservative_result_bound(const OcsExecutionPlanV2& plan) {
    std::uint64_t epoch_count = 0;
    for (const OcsPlaneProgram& program : plan.plane_programs) {
        std::uint64_t next = 0;
        if (!checked_add_u64(epoch_count, program.epochs.size(), next)) {
            fail("result_file_size_limit", "/",
                 "result upper bound exceeds ABI limit");
        }
        epoch_count = next;
    }
    ResultBound bound(contract::kMaxResultFileBytes);
    bound.add(1, 262144);
    bound.add(plan.topology.node_count, 96);
    bound.add(plan.readiness_tokens.size(), 768);
    bound.add(plan.steps.size(), 1536);
    bound.add(plan.flow_groups.size(), 3072);
    bound.add(plan.flows.size(), 6144);
    bound.add(plan.plane_programs.size(), 3072);
    bound.add(epoch_count, 3072);

    std::uint64_t port_count = 0;
    if (!checked_mul_u64(plan.topology.node_count, plan.topology.plane_count,
                         port_count) ||
        !checked_mul_u64(port_count, 2, port_count)) {
        fail("result_file_size_limit", "/",
             "result upper bound exceeds ABI limit");
    }
    bound.add_product(port_count, plan.flows.size(), 192);
    if (bound.exceeded()) {
        fail("result_file_size_limit", "/",
             "result upper bound exceeds ABI limit");
    }
    return bound.value();
}

std::string event_node(std::string_view kind, std::uint64_t id) {
    return std::string(kind) + ":" + std::to_string(id);
}

std::string epoch_node(std::string_view kind, std::uint64_t plane,
                       std::uint64_t epoch) {
    return std::string(kind) + ":" + std::to_string(plane) + ":" +
           std::to_string(epoch);
}

std::uint64_t validate_semantics(OcsExecutionPlanV2& plan) {
    const std::string expected_policy =
        plan.strategy == "baseline"
            ? "step_lockstep"
            : (plan.strategy == "one_shot" ? "static_preinstalled"
                                             : "overlap_earliest");
    if (plan.path_preparation_policy != expected_policy) {
        fail("strategy_policy_mismatch", "/path_preparation_policy",
             "strategy and path preparation policy do not match");
    }
    if (plan.workload.rank_count != plan.topology.node_count) {
        fail("rank_count_mismatch", "/workload/rank_count",
             "workload rank_count must equal topology node_count");
    }

    require_contiguous_ids(
        plan.configurations,
        [](const OcsConfiguration& item) { return item.configuration_id; },
        "/configurations", "configuration_id");
    require_contiguous_ids(
        plan.logical_segments,
        [](const OcsLogicalSegment& item) { return item.segment_id; },
        "/logical_segments", "segment_id");
    require_contiguous_ids(
        plan.readiness_tokens,
        [](const OcsReadinessToken& item) { return item.token_id; },
        "/readiness_tokens", "token_id");
    require_contiguous_ids(
        plan.steps, [](const OcsLogicalStepSpec& item) { return item.step_id; },
        "/steps", "step_id");
    require_contiguous_ids(
        plan.flow_groups,
        [](const OcsFlowGroupSpec& item) { return item.flow_group_id; },
        "/flow_groups", "flow_group_id");
    require_contiguous_ids(
        plan.flows, [](const OcsFlowSpec& item) { return item.flow_id; },
        "/flows", "flow_id");
    require_contiguous_ids(
        plan.plane_programs,
        [](const OcsPlaneProgram& item) { return item.plane_id; },
        "/plane_programs", "plane_id");

    if (plan.plane_programs.size() != plan.topology.plane_count) {
        fail("plane_program_count_mismatch", "/plane_programs",
             "one plane program is required for every plane");
    }
    for (const OcsConfiguration& configuration : plan.configurations) {
        const std::string pointer = "/configurations/" +
                                    std::to_string(configuration.configuration_id) +
                                    "/permutation";
        if (configuration.permutation.size() != plan.topology.node_count) {
            fail("invalid_permutation", pointer,
                 "configuration permutation must be a complete rank bijection");
        }
        std::set<std::uint64_t> seen;
        for (const std::uint64_t rank : configuration.permutation) {
            if (rank >= plan.topology.node_count || !seen.insert(rank).second) {
                fail("invalid_permutation", pointer,
                     "configuration permutation must be a complete rank bijection");
            }
        }
    }

    for (const OcsLogicalSegment& segment : plan.logical_segments) {
        const std::string pointer =
            "/logical_segments/" + std::to_string(segment.segment_id);
        require_sorted_unique(segment.origin_rank_ids,
                              pointer + "/origin_rank_ids");
        require_sorted_unique(segment.final_destination_rank_ids,
                              pointer + "/final_destination_rank_ids");
        for (const std::uint64_t rank : segment.origin_rank_ids) {
            if (rank >= plan.topology.node_count) {
                fail("rank_out_of_range", pointer + "/origin_rank_ids",
                     "segment rank is outside topology");
            }
        }
        for (const std::uint64_t rank : segment.final_destination_rank_ids) {
            if (rank >= plan.topology.node_count) {
                fail("rank_out_of_range",
                     pointer + "/final_destination_rank_ids",
                     "segment rank is outside topology");
            }
        }
        std::uint64_t range_end = 0;
        if (!checked_add_u64(segment.buffer_offset_bytes, segment.length_bytes,
                             range_end)) {
            fail("segment_range_overflow", pointer,
                 "segment byte range overflows uint64");
        }
    }

    if (plan.readiness_tokens.empty() ||
        plan.readiness_tokens[0].token_id != 0 ||
        plan.readiness_tokens[0].token_type != "collective_start" ||
        plan.readiness_tokens[0].producer_id.has_value()) {
        fail("invalid_collective_start_token", "/readiness_tokens/0",
             "token 0 must be the unique collective_start token");
    }
    std::size_t collective_start_count = 0;
    std::vector<std::uint64_t> group_token(plan.flow_groups.size(), kMissingId);
    std::vector<std::uint64_t> step_token(plan.steps.size(), kMissingId);
    for (const OcsReadinessToken& token : plan.readiness_tokens) {
        const std::string pointer =
            "/readiness_tokens/" + std::to_string(token.token_id);
        if (token.token_type == "collective_start") {
            ++collective_start_count;
            if (token.producer_id.has_value()) {
                fail("token_producer_mismatch", pointer,
                     "collective_start has no producer");
            }
        } else if (token.token_type == "flow_group_complete") {
            if (!token.producer_id.has_value() ||
                *token.producer_id >= plan.flow_groups.size()) {
                fail("token_producer_mismatch", pointer + "/producer_id",
                     "invalid group token producer");
            }
            std::uint64_t& slot = group_token[*token.producer_id];
            if (slot != kMissingId) {
                fail("duplicate_token_producer", pointer,
                     "group has more than one completion token");
            }
            slot = token.token_id;
        } else {
            if (!token.producer_id.has_value() ||
                *token.producer_id >= plan.steps.size()) {
                fail("token_producer_mismatch", pointer + "/producer_id",
                     "invalid step token producer");
            }
            std::uint64_t& slot = step_token[*token.producer_id];
            if (slot != kMissingId) {
                fail("duplicate_token_producer", pointer,
                     "step has more than one completion token");
            }
            slot = token.token_id;
        }
    }
    if (collective_start_count != 1) {
        fail("duplicate_collective_start_token", "/readiness_tokens",
             "exactly one collective_start token is required");
    }
    if (std::find(group_token.begin(), group_token.end(), kMissingId) !=
        group_token.end()) {
        fail("missing_group_completion_token", "/readiness_tokens",
             "every group needs exactly one completion token");
    }
    if (std::find(step_token.begin(), step_token.end(), kMissingId) !=
        step_token.end()) {
        fail("missing_step_completion_token", "/readiness_tokens",
             "every step needs exactly one completion token");
    }

    std::vector<std::uint64_t> group_step(plan.flow_groups.size(), kMissingId);
    for (const OcsLogicalStepSpec& step : plan.steps) {
        const std::string pointer =
            "/steps/" + std::to_string(step.step_id);
        require_sorted_unique(step.flow_group_ids, pointer + "/flow_group_ids");
        if (step.completion_token_id != step_token[step.step_id]) {
            fail("step_token_mismatch", pointer + "/completion_token_id",
                 "step completion token does not name this step");
        }
        for (const std::uint64_t group_id : step.flow_group_ids) {
            require_index(group_id, plan.flow_groups.size(),
                          pointer + "/flow_group_ids", "flow group");
            if (group_step[group_id] != kMissingId) {
                fail("duplicate_group_membership", pointer + "/flow_group_ids",
                     "group belongs to more than one step");
            }
            group_step[group_id] = step.step_id;
        }
    }
    if (std::find(group_step.begin(), group_step.end(), kMissingId) !=
        group_step.end()) {
        fail("missing_group_step_membership", "/steps",
             "every group must belong to one step");
    }

    struct EpochMembership {
        std::uint64_t plane_id;
        std::uint64_t epoch_id;
        std::uint64_t configuration_id;
    };
    std::vector<std::optional<EpochMembership>> group_epoch(
        plan.flow_groups.size());
    for (const OcsPlaneProgram& program : plan.plane_programs) {
        require_contiguous_ids(
            program.epochs,
            [](const OcsProgramEpochSpec& item) {
                return item.program_epoch_id;
            },
            "/plane_programs/" + std::to_string(program.plane_id) + "/epochs",
            "program_epoch_id");
        std::optional<std::uint64_t> previous_configuration;
        for (const OcsProgramEpochSpec& epoch : program.epochs) {
            const std::string pointer =
                "/plane_programs/" + std::to_string(program.plane_id) +
                "/epochs/" + std::to_string(epoch.program_epoch_id);
            require_index(epoch.configuration_id, plan.configurations.size(),
                          pointer + "/configuration_id", "configuration");
            require_sorted_unique(epoch.path_prep_not_before_token_ids,
                                  pointer +
                                      "/path_prep_not_before_token_ids");
            require_sorted_unique(epoch.flow_group_ids,
                                  pointer + "/flow_group_ids");
            if (epoch.program_epoch_id == 0 && epoch.transition != "initial") {
                fail("invalid_initial_transition", pointer + "/transition",
                     "first used epoch must be initial");
            }
            if (epoch.program_epoch_id > 0 && epoch.transition == "initial") {
                fail("invalid_later_initial", pointer + "/transition",
                     "only epoch zero may be initial");
            }
            if (epoch.transition == "retain" &&
                epoch.configuration_id != previous_configuration) {
                fail("retain_configuration_mismatch",
                     pointer + "/configuration_id",
                     "retain must keep the previous configuration");
            }
            if (epoch.transition == "reconfigure" &&
                previous_configuration.has_value() &&
                epoch.configuration_id == *previous_configuration) {
                fail("reconfigure_same_configuration",
                     pointer + "/configuration_id",
                     "reconfigure must install a different configuration");
            }
            previous_configuration = epoch.configuration_id;
            for (const std::uint64_t token_id :
                 epoch.path_prep_not_before_token_ids) {
                require_index(token_id, plan.readiness_tokens.size(),
                              pointer + "/path_prep_not_before_token_ids",
                              "token");
            }
            for (const std::uint64_t group_id : epoch.flow_group_ids) {
                require_index(group_id, plan.flow_groups.size(),
                              pointer + "/flow_group_ids", "flow group");
                if (group_epoch[group_id].has_value()) {
                    fail("duplicate_group_epoch_membership",
                         pointer + "/flow_group_ids",
                         "group belongs to more than one plane epoch");
                }
                group_epoch[group_id] = EpochMembership{
                    program.plane_id, epoch.program_epoch_id,
                    epoch.configuration_id};
            }
        }
    }
    if (std::find_if(group_epoch.begin(), group_epoch.end(),
                     [](const auto& item) { return !item.has_value(); }) !=
        group_epoch.end()) {
        fail("missing_group_epoch_membership", "/plane_programs",
             "every group must belong to one plane epoch");
    }

    std::vector<std::uint64_t> flow_group(plan.flows.size(), kMissingId);
    std::map<std::string, std::set<std::string>> event_edges;
    for (const OcsFlowGroupSpec& group : plan.flow_groups) {
        const std::string pointer =
            "/flow_groups/" + std::to_string(group.flow_group_id);
        if (group.step_id >= plan.steps.size() ||
            group_step[group.flow_group_id] != group.step_id) {
            fail("group_step_mismatch", pointer + "/step_id",
                 "group step cross-check failed");
        }
        const EpochMembership membership = *group_epoch[group.flow_group_id];
        if (group.plane_id != membership.plane_id ||
            group.program_epoch_id != membership.epoch_id ||
            group.configuration_id != membership.configuration_id) {
            fail("group_epoch_mismatch", pointer,
                 "group plane/epoch/config cross-check failed");
        }
        if (group.completion_token_id != group_token[group.flow_group_id]) {
            fail("group_token_mismatch", pointer + "/completion_token_id",
                 "group completion token does not name this group");
        }
        require_sorted_unique(group.depends_on_token_ids,
                              pointer + "/depends_on_token_ids");
        require_sorted_unique(group.flow_ids, pointer + "/flow_ids");
        for (const std::uint64_t token_id : group.depends_on_token_ids) {
            require_index(token_id, plan.readiness_tokens.size(),
                          pointer + "/depends_on_token_ids", "token");
            if (token_id >= group.completion_token_id) {
                fail("token_not_topologically_numbered",
                     pointer + "/depends_on_token_ids",
                     "dependency token must precede produced token");
            }
            add_edge(event_edges, event_node("token", token_id),
                     event_node("group_release", group.flow_group_id));
        }
        add_edge(event_edges,
                 epoch_node("epoch_path", group.plane_id,
                            group.program_epoch_id),
                 event_node("group_release", group.flow_group_id));
        add_edge(event_edges, event_node("group_release", group.flow_group_id),
                 event_node("group_complete", group.flow_group_id));
        add_edge(event_edges, event_node("group_complete", group.flow_group_id),
                 event_node("token", group.completion_token_id));
        add_edge(event_edges, event_node("group_complete", group.flow_group_id),
                 event_node("step_complete", group.step_id));
        add_edge(event_edges, event_node("group_complete", group.flow_group_id),
                 epoch_node("epoch_close", group.plane_id,
                            group.program_epoch_id));
        for (const std::uint64_t flow_id : group.flow_ids) {
            require_index(flow_id, plan.flows.size(), pointer + "/flow_ids",
                          "flow");
            if (flow_group[flow_id] != kMissingId) {
                fail("duplicate_flow_membership", pointer + "/flow_ids",
                     "flow belongs to more than one group");
            }
            flow_group[flow_id] = group.flow_group_id;
        }
    }
    if (std::find(flow_group.begin(), flow_group.end(), kMissingId) !=
        flow_group.end()) {
        fail("missing_flow_group_membership", "/flow_groups",
             "every flow must belong to one group");
    }

    for (const OcsLogicalStepSpec& step : plan.steps) {
        add_edge(event_edges, event_node("step_complete", step.step_id),
                 event_node("token", step.completion_token_id));
    }
    for (const OcsPlaneProgram& program : plan.plane_programs) {
        for (const OcsProgramEpochSpec& epoch : program.epochs) {
            if (epoch.program_epoch_id > 0) {
                add_edge(event_edges,
                         epoch_node("epoch_close", program.plane_id,
                                    epoch.program_epoch_id - 1),
                         epoch_node("epoch_path", program.plane_id,
                                    epoch.program_epoch_id));
            }
            for (const std::uint64_t token_id :
                 epoch.path_prep_not_before_token_ids) {
                add_edge(event_edges, event_node("token", token_id),
                         epoch_node("epoch_path", program.plane_id,
                                    epoch.program_epoch_id));
            }
        }
    }

    for (const OcsFlowSpec& flow : plan.flows) {
        const std::string pointer =
            "/flows/" + std::to_string(flow.flow_id);
        if (flow.flow_group_id >= plan.flow_groups.size() ||
            flow_group[flow.flow_id] != flow.flow_group_id) {
            fail("flow_group_mismatch", pointer + "/flow_group_id",
                 "flow group cross-check failed");
        }
        if (flow.src_rank >= plan.topology.node_count ||
            flow.dst_rank >= plan.topology.node_count) {
            fail("rank_out_of_range", pointer,
                 "flow endpoint is outside topology");
        }
        if (flow.src_rank == flow.dst_rank) {
            fail("self_flow", pointer + "/dst_rank",
                 "active network flow cannot be self traffic");
        }
        const OcsFlowGroupSpec& group = plan.flow_groups[flow.flow_group_id];
        require_index(group.configuration_id, plan.configurations.size(),
                      pointer, "configuration");
        const OcsConfiguration& configuration =
            plan.configurations[group.configuration_id];
        if (configuration.permutation[flow.src_rank] != flow.dst_rank) {
            fail("flow_route_mismatch", pointer,
                 "flow destination does not match its configuration");
        }
        std::uint64_t slice_total = 0;
        for (std::size_t slice_index = 0;
             slice_index < flow.segment_slices.size(); ++slice_index) {
            const OcsSegmentSlice& slice = flow.segment_slices[slice_index];
            const std::string slice_pointer =
                pointer + "/segment_slices/" + std::to_string(slice_index);
            require_index(slice.segment_id, plan.logical_segments.size(),
                          slice_pointer + "/segment_id", "segment");
            const OcsLogicalSegment& segment =
                plan.logical_segments[slice.segment_id];
            if (slice.segment_offset_bytes > segment.length_bytes ||
                slice.length_bytes >
                    segment.length_bytes - slice.segment_offset_bytes) {
                fail("segment_slice_out_of_range", slice_pointer,
                     "slice exceeds referenced segment");
            }
            std::uint64_t next = 0;
            if (!checked_add_u64(slice_total, slice.length_bytes, next)) {
                fail("uint64_overflow", pointer + "/segment_slices",
                     "checked uint64 sum overflow");
            }
            slice_total = next;
        }
        if (slice_total != flow.payload_bytes) {
            fail("flow_slice_byte_mismatch", pointer + "/segment_slices",
                 "slice lengths must equal flow payload");
        }
    }
    for (const OcsFlowGroupSpec& group : plan.flow_groups) {
        event_edges.try_emplace(event_node("token", group.completion_token_id));
    }
    require_acyclic(event_edges);

    if (plan.dependency_mode == "global_step_barrier") {
        for (const OcsFlowGroupSpec& group : plan.flow_groups) {
            const std::vector<std::uint64_t> expected =
                group.step_id == 0
                    ? std::vector<std::uint64_t>{0}
                    : std::vector<std::uint64_t>{
                          plan.steps[group.step_id - 1].completion_token_id};
            if (group.depends_on_token_ids != expected) {
                fail("global_barrier_dependency_mismatch",
                     "/flow_groups/" + std::to_string(group.flow_group_id) +
                         "/depends_on_token_ids",
                     "global barrier group has the wrong release token");
            }
        }
    }

    if (plan.path_preparation_policy == "overlap_earliest") {
        for (const OcsPlaneProgram& program : plan.plane_programs) {
            for (const OcsProgramEpochSpec& epoch : program.epochs) {
                if (!epoch.path_prep_not_before_token_ids.empty()) {
                    fail("swot_path_prep_not_earliest",
                         "/plane_programs/" +
                             std::to_string(program.plane_id) + "/epochs/" +
                             std::to_string(epoch.program_epoch_id) +
                             "/path_prep_not_before_token_ids",
                         "SWOT path preparation must not wait for data tokens");
                }
            }
        }
    } else if (plan.path_preparation_policy == "static_preinstalled") {
        for (const OcsPlaneProgram& program : plan.plane_programs) {
            if (!program.epochs.empty() &&
                (program.epochs.size() != 1 ||
                 program.epochs[0].transition != "initial" ||
                 !program.epochs[0]
                      .path_prep_not_before_token_ids.empty())) {
                fail("one_shot_not_static",
                     "/plane_programs/" + std::to_string(program.plane_id) +
                         "/epochs",
                     "one-shot used plane must contain one preinstalled epoch");
            }
        }
    } else {
        for (const OcsPlaneProgram& program : plan.plane_programs) {
            for (const OcsProgramEpochSpec& epoch : program.epochs) {
                std::set<std::uint64_t> group_steps;
                for (const std::uint64_t group_id : epoch.flow_group_ids) {
                    group_steps.insert(plan.flow_groups[group_id].step_id);
                }
                const std::string pointer =
                    "/plane_programs/" + std::to_string(program.plane_id) +
                    "/epochs/" + std::to_string(epoch.program_epoch_id);
                if (group_steps.size() != 1) {
                    fail("baseline_epoch_mixed_steps",
                         pointer + "/flow_group_ids",
                         "baseline epoch can contain only one step");
                }
                const std::uint64_t step_id = *group_steps.begin();
                const std::vector<std::uint64_t> expected =
                    step_id == 0
                        ? std::vector<std::uint64_t>{}
                        : std::vector<std::uint64_t>{
                              plan.steps[step_id - 1].completion_token_id};
                if (epoch.path_prep_not_before_token_ids != expected) {
                    fail("baseline_path_prep_too_early",
                         pointer + "/path_prep_not_before_token_ids",
                         "baseline path preparation must wait for the previous step");
                }
                if (epoch.program_epoch_id == 0 && step_id != 0) {
                    fail("baseline_late_step_preinstalled", pointer,
                         "baseline cannot preinstall a later step");
                }
            }
        }
    }

    const OcsPlannerCertificate& certificate = plan.planner_certificate;
    std::vector<std::uint64_t> schedule_ids;
    for (std::size_t index = 0; index < certificate.nominal_schedule.size();
         ++index) {
        const OcsNominalScheduleEntry& entry =
            certificate.nominal_schedule[index];
        schedule_ids.push_back(entry.flow_group_id);
        require_index(entry.flow_group_id, plan.flow_groups.size(),
                      "/planner_certificate/nominal_schedule/" +
                          std::to_string(index) + "/flow_group_id",
                      "flow group");
        if (entry.planned_release_ps > entry.planned_complete_ps) {
            fail("nominal_schedule_timing_order",
                 "/planner_certificate/nominal_schedule/" +
                     std::to_string(index),
                 "planned release must not follow planned completion");
        }
    }
    require_sorted_unique(schedule_ids,
                          "/planner_certificate/nominal_schedule");
    if (certificate.planner_name == "hand_authored_fixture" &&
        (certificate.solver_name.has_value() ||
         certificate.solver_status != "not_used" ||
         certificate.decision_sha256.has_value() ||
         !certificate.nominal_schedule.empty())) {
        fail("fixture_certificate_mismatch", "/planner_certificate",
             "hand-authored fixture certificate has production fields");
    }
    if (certificate.solver_status == "optimal" &&
        certificate.relative_gap_ppm != std::optional<std::uint64_t>(0)) {
        fail("optimal_gap_nonzero",
             "/planner_certificate/relative_gap_ppm",
             "optimal solver gap must be zero");
    }

    const OcsProvenance& provenance = plan.provenance;
    const std::vector<bool> provenance_presence = {
        provenance.overlap4ocs_git_sha.has_value(),
        provenance.htsim_upstream_git_sha.has_value(),
        provenance.htsim_local_patchset_sha256.has_value(),
        provenance.instance_file_sha256.has_value(),
        provenance.program_file_sha256.has_value(),
        provenance.collective_ir_sha256.has_value(),
        provenance.plan_build_context_sha256.has_value(),
    };
    if (provenance.source_kind == "hand_authored_fixture" &&
        std::any_of(provenance_presence.begin(), provenance_presence.end(),
                    [](bool value) { return value; })) {
        fail("fixture_provenance_nonnull", "/provenance",
             "fixture provenance hashes must be null");
    }
    if (provenance.source_kind == "production") {
        if (std::any_of(provenance_presence.begin(), provenance_presence.end(),
                        [](bool value) { return !value; })) {
            fail("production_provenance_missing", "/provenance",
                 "production provenance hashes are required");
        }
        if (!certificate.decision_sha256.has_value()) {
            fail("production_decision_hash_missing",
                 "/planner_certificate/decision_sha256",
                 "production plan requires the canonical decision digest");
        }
        std::vector<std::uint64_t> expected_ids(plan.flow_groups.size());
        for (std::size_t index = 0; index < expected_ids.size(); ++index) {
            expected_ids[index] = index;
        }
        if (schedule_ids != expected_ids) {
            fail("production_schedule_incomplete",
                 "/planner_certificate/nominal_schedule",
                 "production schedule must contain every flow group");
        }
        if (certificate.solver_status == "not_used") {
            if (certificate.solver_name.has_value()) {
                fail("unused_solver_named", "/planner_certificate/solver_name",
                     "solver_name must be null when no solver was used");
            }
        } else if (!certificate.solver_name.has_value() ||
                   !certificate.objective_ps.has_value()) {
            fail("solver_certificate_incomplete", "/planner_certificate",
                 "solver-backed plan requires solver_name and objective_ps");
        }
        if (!certificate.nominal_schedule.empty() &&
            certificate.solver_status != "not_used") {
            std::uint64_t planned_complete = 0;
            for (const OcsNominalScheduleEntry& entry :
                 certificate.nominal_schedule) {
                planned_complete =
                    std::max(planned_complete, entry.planned_complete_ps);
            }
            if (certificate.objective_ps != planned_complete) {
                fail("objective_schedule_mismatch",
                     "/planner_certificate/objective_ps",
                     "solver objective must equal maximum planned completion");
            }
        }
    }

    const std::uint64_t result_bound = conservative_result_bound(plan);
    DerivedTrafficInventory inventory;
    inventory.expected_flow_count = plan.flows.size();
    inventory.expected_flow_group_count = plan.flow_groups.size();
    inventory.expected_payload_bytes = 0;
    inventory.per_rank_sent_payload_bytes.assign(plan.topology.node_count, 0);
    inventory.per_rank_received_payload_bytes.assign(plan.topology.node_count, 0);
    inventory.per_plane_payload_bytes.assign(plan.topology.plane_count, 0);
    inventory.dependency_children_by_token.resize(plan.readiness_tokens.size());
    for (const OcsFlowGroupSpec& group : plan.flow_groups) {
        for (const std::uint64_t token_id : group.depends_on_token_ids) {
            inventory.dependency_children_by_token[token_id].push_back(
                group.flow_group_id);
        }
    }
    for (const OcsFlowSpec& flow : plan.flows) {
        std::uint64_t next = 0;
        if (!checked_add_u64(inventory.expected_payload_bytes,
                             flow.payload_bytes, next)) {
            fail("uint64_overflow", "/flows",
                 "expected payload byte sum overflows uint64");
        }
        inventory.expected_payload_bytes = next;
        if (!checked_add_u64(
                inventory.per_rank_sent_payload_bytes[flow.src_rank],
                flow.payload_bytes, next)) {
            fail("uint64_overflow", "/flows",
                 "per-rank sent payload sum overflows uint64");
        }
        inventory.per_rank_sent_payload_bytes[flow.src_rank] = next;
        if (!checked_add_u64(
                inventory.per_rank_received_payload_bytes[flow.dst_rank],
                flow.payload_bytes, next)) {
            fail("uint64_overflow", "/flows",
                 "per-rank received payload sum overflows uint64");
        }
        inventory.per_rank_received_payload_bytes[flow.dst_rank] = next;
        const std::uint64_t plane_id =
            plan.flow_groups[flow.flow_group_id].plane_id;
        if (!checked_add_u64(inventory.per_plane_payload_bytes[plane_id],
                             flow.payload_bytes, next)) {
            fail("uint64_overflow", "/flows",
                 "per-plane payload sum overflows uint64");
        }
        inventory.per_plane_payload_bytes[plane_id] = next;
    }
    plan.traffic_inventory = std::move(inventory);
    return result_bound;
}

struct FileSnapshot {
    dev_t device;
    ino_t inode;
    off_t size;
    timespec modified;
};

FileSnapshot snapshot(const struct stat& status) {
    return {status.st_dev, status.st_ino, status.st_size, status.st_mtim};
}

bool same_snapshot(const FileSnapshot& left, const FileSnapshot& right) {
    return left.device == right.device && left.inode == right.inode &&
           left.size == right.size &&
           left.modified.tv_sec == right.modified.tv_sec &&
           left.modified.tv_nsec == right.modified.tv_nsec;
}

std::string read_plan_file(const std::filesystem::path& path) {
    const int descriptor =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        fail("input_not_regular_or_readable", "/",
             "plan is not a readable regular file", ExitCode::kIoOrInternal);
    }
    struct DescriptorGuard {
        int descriptor;
        ~DescriptorGuard() { ::close(descriptor); }
    } guard{descriptor};

    struct stat before_status {};
    if (::fstat(descriptor, &before_status) != 0 ||
        !S_ISREG(before_status.st_mode)) {
        fail("input_not_regular_or_readable", "/",
             "plan is not a readable regular file", ExitCode::kIoOrInternal);
    }
    if (before_status.st_size < 0 ||
        static_cast<std::uint64_t>(before_status.st_size) >
            contract::kMaxPlanFileBytes) {
        fail("plan_file_size_limit", "/",
             "plan exceeds the ABI file-size limit");
    }
    const std::size_t size = static_cast<std::size_t>(before_status.st_size);
    std::string raw(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count =
            ::read(descriptor, raw.data() + offset, size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            fail("input_changed_during_read", "/",
                 "plan changed while it was read", ExitCode::kIoOrInternal);
        }
        offset += static_cast<std::size_t>(count);
    }
    char extra = 0;
    const ssize_t extra_count = ::read(descriptor, &extra, 1);
    struct stat after_status {};
    if (extra_count != 0 || ::fstat(descriptor, &after_status) != 0 ||
        !same_snapshot(snapshot(before_status), snapshot(after_status))) {
        fail("input_changed_during_read", "/",
             "plan changed while it was read", ExitCode::kIoOrInternal);
    }
    return raw;
}

}  // namespace

ParsedExecutionPlan OcsPlanParser::parse_file(
    const std::filesystem::path& path) {
    const std::string raw = read_plan_file(path);
    const std::string raw_digest = sha256_hex(raw);
    if (raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xefU &&
        static_cast<unsigned char>(raw[1]) == 0xbbU &&
        static_cast<unsigned char>(raw[2]) == 0xbfU) {
        fail("utf8_bom", "/", "UTF-8 BOM is forbidden");
    }
    if (!valid_utf8(raw)) {
        fail("invalid_utf8", "/", "plan is not valid UTF-8");
    }
    reject_forbidden_number_tokens(raw);

    Json root;
    try {
        root = Json::parse(raw, nullptr, true, false);
    } catch (const Json::parse_error&) {
        fail("invalid_json", "/", "plan is not strict JSON");
    } catch (const Json::out_of_range&) {
        fail("uint64_out_of_range", "/", "wire integer exceeds uint64");
    }
    WireScanner(raw).scan();
    OcsExecutionPlanV2 plan = typed_plan(root);
    const std::uint64_t result_upper_bound = validate_semantics(plan);
    auto mutable_plan =
        std::make_shared<OcsExecutionPlanV2>(std::move(plan));
    std::shared_ptr<const OcsExecutionPlanV2> immutable_plan = mutable_plan;
    return {
        std::move(immutable_plan),
        {raw_digest, static_cast<std::uint64_t>(raw.size()),
         result_upper_bound},
    };
}

}  // namespace htsim_ocs
