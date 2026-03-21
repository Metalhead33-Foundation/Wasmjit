#ifndef ELVSTRINGHASHMAP_HPP
#define ELVSTRINGHASHMAP_HPP
#include <map>
#include <unordered_map>
#include <string>
#include <string_view>
namespace Elv {
namespace Util {

struct StringHash {
  using is_transparent = void;
  [[nodiscard]] size_t operator()(const char *txt) const {
	return std::hash<std::string_view>{}(txt);
  }
  [[nodiscard]] size_t operator()(std::string_view txt) const {
	return std::hash<std::string_view>{}(txt);
  }
  [[nodiscard]] size_t operator()(const std::string &txt) const {
	return std::hash<std::string>{}(txt);
  }
};

template <typename T> using UnorderedStrMap = std::unordered_map<std::string, T, StringHash, std::equal_to<>>;
template <typename T> using StrMap = std::map<std::string, T, std::less<>>;

}
}
#endif // ELVSTRINGHASHMAP_HPP
