#include "infrastructure/cleanup/stick_containment.hpp"

#include "application/path_key.hpp"

namespace seabass::infrastructure::cleanup
{

bool isUnderStickRoot(const std::string &filePath, const std::string &stickRoot)
{
    if (filePath.empty() || stickRoot.empty()) {
        return false;
    }
    std::string file = application::normalizedPathKey(filePath);
    std::string root = application::normalizedPathKey(stickRoot);
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) {
        root.pop_back();
    }
    if (root.empty() || file.size() <= root.size()) {
        return false;
    }
    return file.compare(0, root.size(), root) == 0 && (file[root.size()] == '/' || file[root.size()] == '\\');
}

}  // namespace seabass::infrastructure::cleanup
