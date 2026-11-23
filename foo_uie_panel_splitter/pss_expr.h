#pragma once

#include <string>

namespace pss {

/// Retrieve a persisted/session PSS global variable (empty string if unset).
std::string get_global(const std::string& name);

/// Set or overwrite a PSS global variable (shared across all PSS instances).
void set_global(const std::string& name, const std::string& value);

/// Evaluate a PSS script or coordinate/dimension expression with %ps_width% / %ps_height%.
std::string eval_expr(const std::string& expr, int ps_width, int ps_height, bool allow_side_effects);

/// Evaluate an integer coordinate/dimension expression (0 on non-numeric/empty).
int eval_int(const std::string& expr, int ps_width, int ps_height);

} // namespace pss
