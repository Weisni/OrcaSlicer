#pragma once

#include "nlohmann/json.hpp"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace Slic3r::HaMaterialContext {
using Json = nlohmann::json;

inline std::string diameter(double value)
{
    if (!std::isfinite(value) || value <= 0 || value > 10)
        throw std::runtime_error("Select a valid nozzle diameter");
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(6) << value;
    auto text = out.str();
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text;
}

inline std::string key(const Json &context)
{
    if (!context.is_object() || context.size() != 3)
        throw std::runtime_error("Invalid nozzle profile context");
    const auto model = context.at("printer_model").get<std::string>();
    const auto size = context.at("nozzle_diameter").get<std::string>();
    const auto flow = context.at("flow_type").get<std::string>();
    if (model.empty() || model.size() > 128 || model.find('|') != std::string::npos ||
        model.find_first_of("\r\n\t") != std::string::npos || model.find('\0') != std::string::npos ||
        size.empty() || size.find_first_not_of("0123456789.") != std::string::npos ||
        (flow != "standard" && flow != "high_flow"))
        throw std::runtime_error("Invalid nozzle profile context");
    std::istringstream input(size);
    input.imbue(std::locale::classic());
    double value = 0;
    input >> value;
    if (!input || input.peek() != std::char_traits<char>::eof() || diameter(value) != size)
        throw std::runtime_error("Nozzle diameter must be canonical");
    return model + "|" + size + "|" + flow;
}

inline Json make(const std::string &model, double size, const std::string &flow)
{
    Json context = {{"printer_model", model}, {"nozzle_diameter", diameter(size)}, {"flow_type", flow}};
    key(context);
    return context;
}
} // namespace Slic3r::HaMaterialContext
