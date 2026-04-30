// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
// Without limiting the foregoing, you may not, at any time or for any
// reason, directly or indirectly, in whole or in part: (i) copy, modify,
// or create derivative works of the Product; (ii) rent, lease, lend, sell,
// sublicense, assign, distribute, publish, transfer, or otherwise make
// available the Product; (iii) reverse engineer, disassemble, decompile,
// decode, or adapt the Product; or (iv) remove any proprietary notices
// from the Product.
#include "core/polynomial_io.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <niobium/fhetch_api.h>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// Walk a JSON tree to find the first "values" array. fhetch's
// save_polynomial_json output structure is opaque to HAZE — using a
// recursive walk keeps us tolerant of layout changes.
const json *find_values_array(const json &node) {
    if (node.is_object()) {
        if (auto it = node.find("values"); it != node.end() && it->is_array()) {
            return &*it;
        }
        for (const auto &kv : node.items()) {
            if (const json *found = find_values_array(kv.value()))
                return found;
        }
    } else if (node.is_array()) {
        for (const auto &v : node) {
            if (const json *found = find_values_array(v))
                return found;
        }
    }
    return nullptr;
}

} // namespace

bool extract_polynomial_values(const fhetch::Polynomial &p, std::string_view tag,
                               std::vector<uint64_t> &out) {
    const auto tmp =
        fs::temp_directory_path() / (std::string("haze_result_") + std::string(tag) + ".json");
    struct RemoveOnExit {
        const fs::path &path;
        ~RemoveOnExit() { fs::remove(path); }
    } cleanup{tmp};

    if (!fhetch::save_polynomial_json(p, tmp))
        return false;

    std::ifstream f(tmp);
    if (!f)
        return false;

    json doc;
    try {
        f >> doc;
    } catch (...) {
        return false;
    }

    const json *values = find_values_array(doc);
    if (values == nullptr)
        return false;
    try {
        out = values->get<std::vector<uint64_t>>();
    } catch (...) {
        return false;
    }
    return !out.empty();
}

} // namespace haze
