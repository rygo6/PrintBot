// print_bot core: error/result types. GUI-free (no wxWidgets).
#ifndef printbot_core_Result_hpp_
#define printbot_core_Result_hpp_

#include <string>
#include <utility>
#include <variant>

namespace printbot::core {

// Stable, machine-readable error. `code` is dotted, e.g. "preset.not_found".
struct Error
{
    std::string code;
    std::string message;
};

inline Error make_error(std::string code, std::string message) { return Error{std::move(code), std::move(message)}; }

template<class T> class Result
{
public:
    Result(T value) : m_v(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : m_v(std::in_place_index<1>, std::move(error)) {}

    bool         ok() const { return m_v.index() == 0; }
    explicit     operator bool() const { return ok(); }
    T&           value() { return std::get<0>(m_v); }
    const T&     value() const { return std::get<0>(m_v); }
    const Error& error() const { return std::get<1>(m_v); }

private:
    std::variant<T, Error> m_v;
};

struct Ok {};
using Status = Result<Ok>;

} // namespace printbot::core

#endif
