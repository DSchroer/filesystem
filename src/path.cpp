//  filesystem path.cpp  -------------------------------------------------------------  //

//  Copyright Beman Dawes 2008

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#include "platform_config.hpp"

#include <boost/filesystem/config.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp> // for filesystem_error
#include <boost/scoped_array.hpp>
#include <boost/system/error_code.hpp>
#include <boost/assert.hpp>
#include <algorithm>
#include <iterator>
#include <utility>
#include <cstddef>
#include <cstring>

#ifdef BOOST_WINDOWS_API
#include "windows_file_codecvt.hpp"
#include <windows.h>
#elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__HAIKU__)
#include <boost/filesystem/detail/utf8_codecvt_facet.hpp>
#endif

#ifdef BOOST_FILESYSTEM_DEBUG
#include <iostream>
#include <iomanip>
#endif

namespace fs = boost::filesystem;

using boost::filesystem::path;

using std::string;
using std::wstring;

using boost::system::error_code;

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                class path helpers                                    //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace {
//------------------------------------------------------------------------------------//
//                        miscellaneous class path helpers                            //
//------------------------------------------------------------------------------------//

typedef path::value_type value_type;
typedef path::string_type string_type;
typedef string_type::size_type size_type;
using boost::filesystem::path_detail::substring;

#ifdef BOOST_WINDOWS_API

const wchar_t separators[] = L"/\\";
const wchar_t separator_string[] = L"/";
const wchar_t preferred_separator_string[] = L"\\";
BOOST_CONSTEXPR_OR_CONST wchar_t colon = L':';
BOOST_CONSTEXPR_OR_CONST wchar_t questionmark = L'?';

inline bool is_letter(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

inline bool is_alnum(wchar_t c)
{
    return is_letter(c) || (c >= L'0' && c <= L'9');
}

inline bool is_device_name_char(wchar_t c)
{
    // https://googleprojectzero.blogspot.com/2016/02/the-definitive-guide-on-win32-to-nt.html
    // Device names are:
    //
    // - PRN
    // - AUX
    // - NUL
    // - CON
    // - LPT[1-9]
    // - COM[1-9]
    // - CONIN$
    // - CONOUT$
    return is_alnum(c) || c == L'$';
}

#else

const char separators[] = "/";
typedef const char* const_char_ptr;
BOOST_CONSTEXPR_OR_CONST const_char_ptr separator_string = separators;
BOOST_CONSTEXPR_OR_CONST const_char_ptr preferred_separator_string = separators;

#endif

// pos is position of the separator
bool is_root_separator(string_type const& str, size_type root_dir_pos, size_type pos);

// Returns: 0 if str itself is filename (or empty)
// end_pos is past-the-end position
size_type filename_pos(string_type const& str, size_type root_name_size, size_type end_pos);

// Returns: starting position of root directory or size if not found. Sets root_name_size to length
// of the root name if the characters before the returned position (if any) are considered a root name.
size_type find_root_directory_start(string_type const& path, size_type size, size_type& root_name_size);

// Finds position and size of the first element of the path
void first_element(string_type const& src, size_type& element_pos, size_type& element_size, size_type size);

// Finds position and size of the first element of the path
inline void first_element(string_type const& src, size_type& element_pos, size_type& element_size)
{
    first_element(src, element_pos, element_size, src.size());
}

} // unnamed namespace

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            class path implementation                                 //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace boost {
namespace filesystem {

BOOST_FILESYSTEM_DECL path& path::operator/=(path const& p)
{
    if (!p.empty())
    {
        if (this == &p) // self-append
        {
            path rhs(p);
            if (!detail::is_directory_separator(rhs.m_pathname[0]))
                append_separator_if_needed();
            m_pathname += rhs.m_pathname;
        }
        else
        {
            if (!detail::is_directory_separator(*p.m_pathname.begin()))
                append_separator_if_needed();
            m_pathname += p.m_pathname;
        }
    }

    return *this;
}

BOOST_FILESYSTEM_DECL path& path::operator/=(const value_type* ptr)
{
    if (*ptr != static_cast< value_type >('\0'))
    {
        if (ptr >= m_pathname.data() && ptr < m_pathname.data() + m_pathname.size()) // overlapping source
        {
            path rhs(ptr);
            if (!detail::is_directory_separator(rhs.m_pathname[0]))
                append_separator_if_needed();
            m_pathname += rhs.m_pathname;
        }
        else
        {
            if (!detail::is_directory_separator(*ptr))
                append_separator_if_needed();
            m_pathname += ptr;
        }
    }

    return *this;
}

#ifdef BOOST_WINDOWS_API

BOOST_FILESYSTEM_DECL path path::generic_path() const
{
    path tmp(*this);
    std::replace(tmp.m_pathname.begin(), tmp.m_pathname.end(), L'\\', L'/');
    return tmp;
}

#endif // BOOST_WINDOWS_API

BOOST_FILESYSTEM_DECL int path::compare(path const& p) const BOOST_NOEXCEPT
{
    return detail::lex_compare(begin(), end(), p.begin(), p.end());
}

//  append_separator_if_needed  ----------------------------------------------------//

BOOST_FILESYSTEM_DECL path::string_type::size_type path::append_separator_if_needed()
{
    if (!m_pathname.empty() &&
#ifdef BOOST_WINDOWS_API
        *(m_pathname.end() - 1) != colon &&
#endif
        !detail::is_directory_separator(*(m_pathname.end() - 1)))
    {
        string_type::size_type tmp(m_pathname.size());
        m_pathname += preferred_separator;
        return tmp;
    }
    return 0;
}

//  erase_redundant_separator  -----------------------------------------------------//

BOOST_FILESYSTEM_DECL void path::erase_redundant_separator(string_type::size_type sep_pos)
{
    if (sep_pos                                  // a separator was added
        && sep_pos < m_pathname.size()           // and something was appended
        && (m_pathname[sep_pos + 1] == separator // and it was also separator
#ifdef BOOST_WINDOWS_API
            || m_pathname[sep_pos + 1] == preferred_separator // or preferred_separator
#endif
            ))
    {
        m_pathname.erase(m_pathname.begin() + sep_pos); // erase the added separator
    }
}

//  modifiers  -----------------------------------------------------------------------//

#ifdef BOOST_WINDOWS_API
BOOST_FILESYSTEM_DECL path& path::make_preferred()
{
    std::replace(m_pathname.begin(), m_pathname.end(), L'/', L'\\');
    return *this;
}
#endif

BOOST_FILESYSTEM_DECL path& path::remove_filename()
{
    size_type end_pos = find_parent_path_size();
    m_pathname.erase(m_pathname.begin() + end_pos, m_pathname.end());
    return *this;
}

BOOST_FILESYSTEM_DECL path& path::remove_trailing_separator()
{
    if (!m_pathname.empty() && detail::is_directory_separator(m_pathname[m_pathname.size() - 1]))
        m_pathname.erase(m_pathname.end() - 1);
    return *this;
}

BOOST_FILESYSTEM_DECL path& path::replace_extension(path const& new_extension)
{
    // erase existing extension, including the dot, if any
    m_pathname.erase(m_pathname.size() - extension().m_pathname.size());

    if (!new_extension.empty())
    {
        // append new_extension, adding the dot if necessary
        if (new_extension.m_pathname[0] != dot)
            m_pathname.push_back(dot);
        m_pathname.append(new_extension.m_pathname);
    }

    return *this;
}

//  decomposition  -------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL size_type path::find_root_name_size() const
{
    size_type root_name_size = 0;
    find_root_directory_start(m_pathname, m_pathname.size(), root_name_size);
    return root_name_size;
}

BOOST_FILESYSTEM_DECL size_type path::find_root_path_size() const
{
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(m_pathname, m_pathname.size(), root_name_size);

    size_type size = root_name_size;
    if (root_dir_pos < m_pathname.size())
        size = root_dir_pos + 1;

    return size;
}

BOOST_FILESYSTEM_DECL substring path::find_root_directory() const
{
    substring root_dir;
    size_type root_name_size = 0;
    root_dir.pos = find_root_directory_start(m_pathname, m_pathname.size(), root_name_size);
    root_dir.size = static_cast< std::size_t >(root_dir.pos < m_pathname.size());
    return root_dir;
}

BOOST_FILESYSTEM_DECL substring path::find_relative_path() const
{
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(m_pathname, m_pathname.size(), root_name_size);

    // Skip root name, root directory and any duplicate separators
    size_type size = root_name_size;
    if (root_dir_pos < m_pathname.size())
    {
        size = root_dir_pos + 1;

        for (size_type n = m_pathname.size(); size < n; ++size)
        {
            if (!detail::is_directory_separator(m_pathname[size]))
                break;
        }
    }

    substring rel_path;
    rel_path.pos = size;
    rel_path.size = m_pathname.size() - size;

    return rel_path;
}

BOOST_FILESYSTEM_DECL string_type::size_type path::find_parent_path_size() const
{
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(m_pathname, m_pathname.size(), root_name_size);

    size_type end_pos = filename_pos(m_pathname, root_name_size, m_pathname.size());

    bool filename_was_separator = !m_pathname.empty() && detail::is_directory_separator(m_pathname[end_pos]);

    // skip separators unless root directory
    for (;
         end_pos > 0 && (end_pos - 1) != root_dir_pos && detail::is_directory_separator(m_pathname[end_pos - 1]);
         --end_pos)
    {
    }

    if (end_pos == 1 && root_dir_pos == 0 && filename_was_separator)
        end_pos = 0;

    return end_pos;
}

BOOST_FILESYSTEM_DECL path path::filename() const
{
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(m_pathname, m_pathname.size(), root_name_size);
    size_type pos = filename_pos(m_pathname, root_name_size, m_pathname.size());
    if (pos < root_name_size)
        return path();
    if (!m_pathname.empty() && pos && detail::is_directory_separator(m_pathname[pos]) && !is_root_separator(m_pathname, root_dir_pos, pos))
        return detail::dot_path();
    return path(m_pathname.c_str() + pos, m_pathname.c_str() + m_pathname.size());
}

BOOST_FILESYSTEM_DECL bool path::has_filename() const
{
    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(m_pathname, m_pathname.size(), root_name_size);
    size_type pos = filename_pos(m_pathname, root_name_size, m_pathname.size());
    return pos >= root_name_size && pos < m_pathname.size();
}

BOOST_FILESYSTEM_DECL path path::stem() const
{
    path name(filename());
    if (name == detail::dot_path() || name == detail::dot_dot_path())
        return name;
    size_type pos = name.m_pathname.rfind(dot);
    return pos == string_type::npos ? name : path(name.m_pathname.c_str(), name.m_pathname.c_str() + pos);
}

BOOST_FILESYSTEM_DECL path path::extension() const
{
    path name(filename());
    if (name == detail::dot_path() || name == detail::dot_dot_path())
        return path();
    size_type pos = name.m_pathname.rfind(dot);
    return pos == string_type::npos ? path() : path(name.m_pathname.c_str() + pos, name.m_pathname.c_str() + name.m_pathname.size());
}

//  lexical operations  --------------------------------------------------------------//

namespace detail {
// C++14 provides a mismatch algorithm with four iterator arguments(), but earlier
// standard libraries didn't, so provide this needed functionality.
inline std::pair< path::iterator, path::iterator > mismatch(path::iterator it1, path::iterator it1end, path::iterator it2, path::iterator it2end)
{
    for (; it1 != it1end && it2 != it2end && *it1 == *it2;)
    {
        ++it1;
        ++it2;
    }
    return std::make_pair(it1, it2);
}
} // namespace detail

BOOST_FILESYSTEM_DECL path path::lexically_relative(path const& base) const
{
    path::iterator b = begin(), e = end(), base_b = base.begin(), base_e = base.end();
    std::pair< path::iterator, path::iterator > mm = detail::mismatch(b, e, base_b, base_e);
    if (mm.first == b && mm.second == base_b)
        return path();
    if (mm.first == e && mm.second == base_e)
        return detail::dot_path();

    std::ptrdiff_t n = 0;
    for (; mm.second != base_e; ++mm.second)
    {
        path const& p = *mm.second;
        if (p == detail::dot_dot_path())
            --n;
        else if (!p.empty() && p != detail::dot_path())
            ++n;
    }
    if (n < 0)
        return path();
    if (n == 0 && (mm.first == e || mm.first->empty()))
        return detail::dot_path();

    path tmp;
    for (; n > 0; --n)
        tmp /= detail::dot_dot_path();
    for (; mm.first != e; ++mm.first)
        tmp /= *mm.first;
    return tmp;
}

//  normal  --------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL path path::lexically_normal() const
{
    if (m_pathname.empty())
        return *this;

    path temp;
    iterator start(begin());
    iterator last(end());
    iterator stop(last--);
    for (iterator itr(start); itr != stop; ++itr)
    {
        // ignore "." except at start and last
        if (itr->native().size() == 1 && (itr->native())[0] == dot && itr != start && itr != last)
            continue;

        // ignore a name and following ".."
        if (!temp.empty() && itr->native().size() == 2 && (itr->native())[0] == dot && (itr->native())[1] == dot) // dot dot
        {
            string_type lf(temp.filename().native());
            string_type::size_type lf_size = lf.size();
            if (lf_size > 0 && (lf_size != 1 || (lf[0] != dot && lf[0] != separator)) && (lf_size != 2 || (lf[0] != dot && lf[1] != dot
#ifdef BOOST_WINDOWS_API
                                                                                                           && lf[1] != colon
#endif
                                                                                                           )))
            {
                temp.remove_filename();
                //// if not root directory, must also remove "/" if any
                //if (temp.native().size() > 0
                //  && temp.native()[temp.native().size()-1]
                //    == separator)
                //{
                //  size_type rns = 0, rds =
                //    find_root_directory_start(temp.native(), temp.native().size(), rns);
                //  if (rds == string_type::npos
                //    || rds != temp.native().size()-1)
                //  {
                //    temp.m_pathname.erase(temp.native().size()-1);
                //  }
                //}

                iterator next(itr);
                if (temp.empty() && ++next != stop && next == last && *last == detail::dot_path())
                {
                    temp /= detail::dot_path();
                }
                continue;
            }
        }

        temp /= *itr;
    }

    if (temp.empty())
        temp /= detail::dot_path();
    return temp;
}

} // namespace filesystem
} // namespace boost

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                         class path helpers implementation                            //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace {

//  is_root_separator  ---------------------------------------------------------------//

// pos is position of the separator
bool is_root_separator(string_type const& str, size_type root_dir_pos, size_type pos)
{
    BOOST_ASSERT_MSG(pos < str.size() && fs::detail::is_directory_separator(str[pos]), "precondition violation");

    // root_dir_pos points at the leftmost separator, we need to skip any duplicate separators right of root dir
    while (pos > root_dir_pos && fs::detail::is_directory_separator(str[pos - 1]))
        --pos;

    return pos == root_dir_pos;
}

//  filename_pos  --------------------------------------------------------------------//

// end_pos is past-the-end position
// return 0 if str itself is filename (or empty)
size_type filename_pos(string_type const& str, size_type root_name_size, size_type end_pos)
{
    if (end_pos <= root_name_size)
        return 0;

    BOOST_ASSERT(end_pos > 0);

    size_type pos = end_pos - 1;
    // case: ends in "/" - return position of the trailing "/" as the filename position
    if (!fs::detail::is_directory_separator(str[pos]) && pos > root_name_size)
    {
        --pos;

        while (true)
        {
            if (fs::detail::is_directory_separator(str[pos]))
            {
                ++pos; // filename starts past the separator
                break;
            }

            if (pos == root_name_size)
                break;

            --pos;
        }
    }

    return pos;
}

//  find_root_directory_start  ------------------------------------------------------------//

// Returns: starting position of root directory or size if not found
size_type find_root_directory_start(string_type const& path, size_type size, size_type& root_name_size)
{
    root_name_size = 0;
    if (size == 0)
        return 0;

    bool parsing_root_name = false;
    size_type pos = 0;

    // case "//", possibly followed by more characters
    if (fs::detail::is_directory_separator(path[0]))
    {
        if (size >= 2 && fs::detail::is_directory_separator(path[1]))
        {
            if (size == 2)
            {
                // The whole path is just a pair of separators
                root_name_size = 2;
                return 2;
            }
#ifdef BOOST_WINDOWS_API
            // https://docs.microsoft.com/en-us/windows/win32/fileio/naming-a-file
            // cases "\\?\" and "\\.\"
            else if (size >= 4 && (path[2] == questionmark || path[2] == fs::path::dot) && fs::detail::is_directory_separator(path[3]))
            {
                parsing_root_name = true;
                pos += 4;
            }
#endif
            else if (fs::detail::is_directory_separator(path[2]))
            {
                // The path starts with three directory separators, which is interpreted as a root directory followed by redundant separators
                return 0;
            }
            else
            {
                // case "//net {/}"
                parsing_root_name = true;
                pos += 2;
                goto find_next_separator;
            }
        }
#ifdef BOOST_WINDOWS_API
        // https://stackoverflow.com/questions/23041983/path-prefixes-and
        // case "\??\" (NT path prefix)
        else if (size >= 4 && path[1] == questionmark && path[2] == questionmark && fs::detail::is_directory_separator(path[3]))
        {
            parsing_root_name = true;
            pos += 4;
        }
#endif
        else
        {
            // The path starts with a separator, possibly followed by a non-separator character
            return 0;
        }
    }

#ifdef BOOST_WINDOWS_API
    // case "c:" or "prn:"
    // Note: There is ambiguity in a "c:x" path interpretation. It could either mean a file "x" located at the current directory for drive C:,
    //       or an alternative stream "x" of a file "c". Windows API resolve this as the former, and so do we.
    if ((size - pos) >= 2 && is_letter(path[pos]))
    {
        size_type i = pos + 1;
        for (; i < size; ++i)
        {
            if (!is_device_name_char(path[i]))
                break;
        }
        
        if (i < size && path[i] == colon)
        {
            pos = i + 1;
            root_name_size = pos;
            parsing_root_name = false;
            
            if (pos < size && fs::detail::is_directory_separator(path[pos]))
                return pos;
        }
    }
#endif

    if (!parsing_root_name)
        return size;

find_next_separator:
    pos = path.find_first_of(separators, pos);
    if (pos > size)
        pos = size;
    if (parsing_root_name)
        root_name_size = pos;

    return pos;
}

//  first_element --------------------------------------------------------------------//

//   sets pos and len of first element, excluding extra separators
//   if src.empty(), sets pos,len, to 0,0.
void first_element(string_type const& src, size_type& element_pos, size_type& element_size, size_type size)
{
    element_pos = 0;
    element_size = 0;
    if (src.empty())
        return;

    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(src, size, root_name_size);

    // First element is the root name, if there is one
    if (root_name_size > 0)
    {
        element_size = root_name_size;
        return;
    }

    // Otherwise, the root directory
    if (root_dir_pos < size)
    {
        element_pos = root_dir_pos;
        element_size = 1u;
        return;
    }

    // Otherwise, the first filename or directory name in a relative path
    size_type end_pos = src.find_first_of(separators);
    if (end_pos == string_type::npos)
        end_pos = src.size();
    element_size = end_pos;
}

} // unnamed namespace

namespace boost {
namespace filesystem {
namespace detail {

BOOST_FILESYSTEM_DECL
int lex_compare(path::iterator first1, path::iterator last1, path::iterator first2, path::iterator last2)
{
    for (; first1 != last1 && first2 != last2;)
    {
        if (first1->native() < first2->native())
            return -1;
        if (first2->native() < first1->native())
            return 1;
        BOOST_ASSERT(first2->native() == first1->native());
        ++first1;
        ++first2;
    }
    if (first1 == last1 && first2 == last2)
        return 0;
    return first1 == last1 ? -1 : 1;
}

BOOST_FILESYSTEM_DECL
path const& dot_path()
{
#ifdef BOOST_WINDOWS_API
    static const fs::path dot_pth(L".");
#else
    static const fs::path dot_pth(".");
#endif
    return dot_pth;
}

BOOST_FILESYSTEM_DECL
path const& dot_dot_path()
{
#ifdef BOOST_WINDOWS_API
    static const fs::path dot_dot(L"..");
#else
    static const fs::path dot_dot("..");
#endif
    return dot_dot;
}

} // namespace detail

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                        class path::iterator implementation                           //
//                                                                                      //
//--------------------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL path::iterator path::begin() const
{
    iterator itr;
    itr.m_path_ptr = this;

    size_type element_size;
    first_element(m_pathname, itr.m_pos, element_size);

    if (element_size > 0)
    {
        itr.m_element = m_pathname.substr(itr.m_pos, element_size);
#ifdef BOOST_WINDOWS_API
        if (itr.m_element.m_pathname == preferred_separator_string)
            itr.m_element.m_pathname = separator_string;
#endif
    }

    return itr;
}

BOOST_FILESYSTEM_DECL path::iterator path::end() const
{
    iterator itr;
    itr.m_path_ptr = this;
    itr.m_pos = m_pathname.size();
    return itr;
}

BOOST_FILESYSTEM_DECL void path::iterator::increment()
{
    BOOST_ASSERT_MSG(m_pos < m_path_ptr->m_pathname.size(), "path::iterator increment past end()");

    // increment to position past current element; if current element is implicit dot,
    // this will cause m_pos to represent the end iterator
    m_pos += m_element.m_pathname.size();

    // if the end is reached, we are done
    if (m_pos >= m_path_ptr->m_pathname.size())
    {
        BOOST_ASSERT_MSG(m_pos == m_path_ptr->m_pathname.size(), "path::iterator increment after the referenced path was modified");
        m_element.clear(); // aids debugging
        return;
    }

    // process separator (Windows drive spec is only case not a separator)
    if (detail::is_directory_separator(m_path_ptr->m_pathname[m_pos]))
    {
        size_type root_name_size = 0;
        size_type root_dir_pos = find_root_directory_start(m_path_ptr->m_pathname, m_path_ptr->m_pathname.size(), root_name_size);

        // detect root directory and set iterator value to the separator if it is
        if (m_pos == root_dir_pos && m_element.m_pathname.size() == root_name_size)
        {
            m_element.m_pathname = separator; // generic format; see docs
            return;
        }

        // skip separators until m_pos points to the start of the next element
        while (m_pos != m_path_ptr->m_pathname.size() && detail::is_directory_separator(m_path_ptr->m_pathname[m_pos]))
        {
            ++m_pos;
        }

        // detect trailing separator, and treat it as ".", per POSIX spec
        if (m_pos == m_path_ptr->m_pathname.size())
        {
            if (!is_root_separator(m_path_ptr->m_pathname, root_dir_pos, m_pos - 1))
            {
                --m_pos;
                m_element = detail::dot_path();
                return;
            }
        }
    }

    // get m_element
    size_type end_pos = m_path_ptr->m_pathname.find_first_of(separators, m_pos);
    if (end_pos == string_type::npos)
        end_pos = m_path_ptr->m_pathname.size();
    m_element = m_path_ptr->m_pathname.substr(m_pos, end_pos - m_pos);
}

BOOST_FILESYSTEM_DECL void path::iterator::decrement()
{
    BOOST_ASSERT_MSG(m_pos > 0, "path::iterator decrement past begin()");
    BOOST_ASSERT_MSG(m_pos <= m_path_ptr->m_pathname.size(), "path::iterator decrement after the referenced path was modified");

    size_type root_name_size = 0;
    size_type root_dir_pos = find_root_directory_start(m_path_ptr->m_pathname, m_path_ptr->m_pathname.size(), root_name_size);

    size_type end_pos = m_pos;

    // if at end and there was a trailing non-root '/', return "."
    if (m_pos == m_path_ptr->m_pathname.size() &&
        m_path_ptr->m_pathname.size() > 1 &&
        detail::is_directory_separator(m_path_ptr->m_pathname[m_pos - 1]) &&
        !is_root_separator(m_path_ptr->m_pathname, root_dir_pos, m_pos - 1))
    {
        --m_pos;
        m_element = detail::dot_path();
        return;
    }

    // skip separators unless root directory
    for (;
        end_pos > 0 && (end_pos - 1) != root_dir_pos && detail::is_directory_separator(m_path_ptr->m_pathname[end_pos - 1]);
        --end_pos)
    {
    }

    m_pos = filename_pos(m_path_ptr->m_pathname, root_name_size, end_pos);
    m_element = m_path_ptr->m_pathname.substr(m_pos, end_pos - m_pos);
    if (m_element.m_pathname == preferred_separator_string) // needed for Windows, harmless on POSIX
        m_element.m_pathname = separator_string;            // generic format; see docs
}

} // namespace filesystem
} // namespace boost

namespace {

//------------------------------------------------------------------------------------//
//                                locale helpers                                      //
//------------------------------------------------------------------------------------//

//  Prior versions of these locale and codecvt implementations tried to take advantage
//  of static initialization where possible, kept a local copy of the current codecvt
//  facet (to avoid codecvt() having to call use_facet()), and was not multi-threading
//  safe (again for efficiency).
//
//  This was error prone, and required different implementation techniques depending
//  on the compiler and also whether static or dynamic linking was used. Furthermore,
//  users could not easily provide their multi-threading safe wrappers because the
//  path interface requires the implementation itself to call codecvt() to obtain the
//  default facet, and the initialization of the static within path_locale() could race.
//
//  The code below is portable to all platforms, is much simpler, and hopefully will be
//  much more robust. Timing tests (on Windows, using a Visual C++ release build)
//  indicated the current code is roughly 9% slower than the previous code, and that
//  seems a small price to pay for better code that is easier to use.

std::locale default_locale()
{
#if defined(BOOST_WINDOWS_API)
    std::locale global_loc = std::locale();
    return std::locale(global_loc, new windows_file_codecvt);
#elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__HAIKU__)
    // "All BSD system functions expect their string parameters to be in UTF-8 encoding
    // and nothing else." See
    // http://developer.apple.com/mac/library/documentation/MacOSX/Conceptual/BPInternational/Articles/FileEncodings.html
    //
    // "The kernel will reject any filename that is not a valid UTF-8 string, and it will
    // even be normalized (to Unicode NFD) before stored on disk, at least when using HFS.
    // The right way to deal with it would be to always convert the filename to UTF-8
    // before trying to open/create a file." See
    // http://lists.apple.com/archives/unix-porting/2007/Sep/msg00023.html
    //
    // "How a file name looks at the API level depends on the API. Current Carbon APIs
    // handle file names as an array of UTF-16 characters; POSIX ones handle them as an
    // array of UTF-8, which is why UTF-8 works well in Terminal. How it's stored on disk
    // depends on the disk format; HFS+ uses UTF-16, but that's not important in most
    // cases." See
    // http://lists.apple.com/archives/applescript-users/2002/Sep/msg00319.html
    //
    // Many thanks to Peter Dimov for digging out the above references!

    std::locale global_loc = std::locale();
    return std::locale(global_loc, new boost::filesystem::detail::utf8_codecvt_facet);
#else // Other POSIX
    // ISO C calls std::locale("") "the locale-specific native environment", and this
    // locale is the default for many POSIX-based operating systems such as Linux.
    return std::locale("");
#endif
}

// std::locale("") construction, needed on non-Apple POSIX systems, can throw
// (if environmental variables LC_MESSAGES or LANG are wrong, for example), so
// path_locale() provides lazy initialization via a local static to ensure that any
// exceptions occur after main() starts and so can be caught. Furthermore,
// path_locale() is only called if path::codecvt() or path::imbue() are themselves
// actually called, ensuring that an exception will only be thrown if std::locale("")
// is really needed.
std::locale& path_locale()
{
    // [locale] paragraph 6: Once a facet reference is obtained from a locale object by
    // calling use_facet<>, that reference remains usable, and the results from member
    // functions of it may be cached and re-used, as long as some locale object refers
    // to that facet.
    static std::locale loc(default_locale());
#ifdef BOOST_FILESYSTEM_DEBUG
    std::cout << "***** path_locale() called" << std::endl;
#endif
    return loc;
}

} // unnamed namespace

//--------------------------------------------------------------------------------------//
//              path::codecvt() and path::imbue() implementation                        //
//--------------------------------------------------------------------------------------//

namespace boost {
namespace filesystem {

BOOST_FILESYSTEM_DECL path::codecvt_type const& path::codecvt()
{
#ifdef BOOST_FILESYSTEM_DEBUG
    std::cout << "***** path::codecvt() called" << std::endl;
#endif
    BOOST_ASSERT_MSG(&path_locale(), "boost::filesystem::path locale initialization error");

    return std::use_facet< std::codecvt< wchar_t, char, std::mbstate_t > >(path_locale());
}

BOOST_FILESYSTEM_DECL std::locale path::imbue(std::locale const& loc)
{
#ifdef BOOST_FILESYSTEM_DEBUG
    std::cout << "***** path::imbue() called" << std::endl;
#endif
    std::locale temp(path_locale());
    path_locale() = loc;
    return temp;
}

} // namespace filesystem
} // namespace boost
