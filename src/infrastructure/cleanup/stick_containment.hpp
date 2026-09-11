#pragma once

#include <string>

namespace seabass::infrastructure::cleanup
{

// True when `filePath` lies under `stickRoot` (normalised, so spelling,
// case and separators do not matter, and the root itself does not
// count). The one rule every deletion on a stick is checked against,
// whoever built the list: a manifest records absolute paths, and a
// mount point can move underneath it.
bool isUnderStickRoot(const std::string &filePath, const std::string &stickRoot);

}  // namespace seabass::infrastructure::cleanup
