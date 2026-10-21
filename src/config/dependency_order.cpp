#include "dependency_order.h"

#include "string_utils.h"

#include <functional>

bool dependency_order(const std::vector<dependency_node_t> &nodes,
                      std::vector<size_t> *order, std::wstring *error) {
  if (! order) return false;
  order->clear();
  std::vector<unsigned char> state(nodes.size(), 0);
  std::function<bool(size_t)> visit = [&](size_t index) {
    if (state[index] == 2) return true;
    if (state[index] == 1) {
      if (error) *error = L"dependency cycle includes " + nodes[index].name;
      return false;
    }
    state[index] = 1;
    for (const std::wstring &required : nodes[index].dependencies) {
      for (size_t candidate = 0; candidate < nodes.size(); ++candidate) {
        if (strings_equal_ignore_case(required.c_str(),
                                      nodes[candidate].name.c_str()) &&
            ! visit(candidate)) {
          return false;
        }
      }
    }
    state[index] = 2;
    order->push_back(index);
    return true;
  };
  for (size_t index = 0; index < nodes.size(); ++index) {
    for (size_t other = index + 1; other < nodes.size(); ++other) {
      if (strings_equal_ignore_case(nodes[index].name.c_str(),
                                    nodes[other].name.c_str())) {
        if (error) *error = L"duplicate service " + nodes[index].name;
        return false;
      }
    }
    if (! visit(index)) return false;
  }
  return true;
}
