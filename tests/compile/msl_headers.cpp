// Compiles the reconstructed Metrowerks C++ library headers on their own and uses the
// containers the way the game does. The reconstruction only has the members the game
// calls, so this is not a conformance test.
#include <algorithm>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct Base { virtual ~Base() {} int v; };
struct Derived : Base, std::tr1::enable_shared_from_this<Derived> {};
static void Free(void*) throw() {}

int msl_headers_test() {
    std::vector<int> vi; vi.push_back(1); vi.resize(4); vi.reserve(9); vi.clear();
    std::vector<std::string> vs; vs.push_back("a");
    std::string s("abc"); s += "d"; s = s + s; s.c_str();
    std::wstring w(L"abc"); w += L"d"; w.size();
    std::list<int> l; l.push_back(1);
    std::map<int, std::string> m; m[1] = "x"; m.find(1);
    std::set<int> st; st.insert(1); st.find(1);
    std::tr1::shared_ptr<Base> p(new Derived);
    std::tr1::shared_ptr<Derived> pd = std::tr1::static_pointer_cast<Derived>(p);
    std::tr1::weak_ptr<Base> wp(p); std::tr1::shared_ptr<Base> locked = wp.lock();
    std::tr1::shared_ptr<char> buf(static_cast<char*>(0), Free); buf.reset();
    return std::min(1, 2) + std::max(3, 4) + (int)vi.size() + pd->v;
}
