#ifndef IBTREE_H
#define IBTREE_H

#include "file.hpp"
#include "shared_node.hpp"

namespace compio {

class IBTree {
public:
    virtual ~IBTree() = default;

    virtual void insert(tree_key key, tree_val value) = 0;
    virtual void remove(tree_key key) = 0;
    virtual void get_range(tree_key key_min, tree_key key_max, std::vector<std::pair<tree_key, tree_val>>& result) = 0;
    virtual bool update(tree_key key, tree_val new_value) = 0;
};

} // namespace compio

#endif //IBTREE_H
