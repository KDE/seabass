#include "infrastructure/cleanup/stick_containment.hpp"

#include "application/path_key.hpp"
#include "application/stick_path_match.hpp"

namespace seabass::infrastructure::cleanup
{

bool isUnderStickRoot(const std::string &filePath, const std::string &stickRoot)
{
    if (filePath.empty() || stickRoot.empty()) {
        return false;
    }
    // normalizedPathKey folds separators and case, so the one separator
    // rule left is pathIsUnder's: the root itself is not under the root.
    const std::string file = application::normalizedPathKey(filePath);
    std::string root = application::normalizedPathKey(stickRoot);
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }
    return file != root && application::pathIsUnder(file, root);
}

}  // namespace seabass::infrastructure::cleanup
