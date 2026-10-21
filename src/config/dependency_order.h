#ifndef SVP_DEPENDENCY_ORDER_H
#define SVP_DEPENDENCY_ORDER_H

#include <string>
#include <vector>

struct dependency_node_t {
  std::wstring name;
  std::vector<std::wstring> dependencies;
};

bool dependency_order(const std::vector<dependency_node_t> &,
                      std::vector<size_t> *, std::wstring *);

#endif
