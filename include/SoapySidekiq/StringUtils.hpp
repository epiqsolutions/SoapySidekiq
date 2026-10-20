/**
 * @file StringUtils.hpp
 * @brief Declares the internal case-insensitive settings/source comparison.
 */

#pragma once

#include <string>

/**
 * Compare settings or source names using the existing character-folding policy.
 * @param a First string.
 * @param b Second string.
 * @return True when lengths and folded characters match.
 */
bool equalsIgnoreCase(const std::string &a, const std::string &b);
