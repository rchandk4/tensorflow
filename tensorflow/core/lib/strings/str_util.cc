#include "tensorflow/core/lib/strings/str_util.h"
#include <ctype.h>

namespace tensorflow {
namespace str_util {

static char hex_char[] = "0123456789abcdef";

string CEscape(const string& src) {
  string dest;

  for (unsigned char c : src) {
    switch (c) {
      case '\n':
        dest.append("\\n");
        break;
      case '\r':
        dest.append("\\r");
        break;
      case '\t':
        dest.append("\\t");
        break;
      case '\"':
        dest.append("\\\"");
        break;
      case '\'':
        dest.append("\\'");
        break;
      case '\\':
        dest.append("\\\\");
        break;
      default:
        // Note that if we emit \xNN and the src character after that is a hex
        // digit then that digit must be escaped too to prevent it being
        // interpreted as part of the character code by C.
        if ((c >= 0x80) || !isprint(c)) {
          dest.append("\\");
          dest.push_back(hex_char[c / 64]);
          dest.push_back(hex_char[(c % 64) / 8]);
          dest.push_back(hex_char[c % 8]);
        } else {
          dest.push_back(c);
          break;
        }
    }
  }

  return dest;
}

namespace {  // Private helpers for CUnescape().

inline bool is_octal_digit(unsigned char c) { return c >= '0' && c <= '7'; }

inline bool ascii_isxdigit(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

inline int hex_digit_to_int(char c) {
  int x = static_cast<unsigned char>(c);
  if (x > '9') {
    x += 9;
  }
  return x & 0xf;
}

bool CUnescapeInternal(StringPiece source, char* dest, int* dest_len,
                       string* error) {
  char* d = dest;
  const char* p = source.data();
  const char* end = source.end();
  const char* last_byte = end - 1;

  // Small optimization for case where source = dest and there's no escaping
  while (p == d && p < end && *p != '\\') p++, d++;

  while (p < end) {
    if (*p != '\\') {
      *d++ = *p++;
    } else {
      if (++p > last_byte) {  // skip past the '\\'
        if (error) *error = "String cannot end with \\";
        return false;
      }
      switch (*p) {
        case 'a':
          *d++ = '\a';
          break;
        case 'b':
          *d++ = '\b';
          break;
        case 'f':
          *d++ = '\f';
          break;
        case 'n':
          *d++ = '\n';
          break;
        case 'r':
          *d++ = '\r';
          break;
        case 't':
          *d++ = '\t';
          break;
        case 'v':
          *d++ = '\v';
          break;
        case '\\':
          *d++ = '\\';
          break;
        case '?':
          *d++ = '\?';
          break;  // \?  Who knew?
        case '\'':
          *d++ = '\'';
          break;
        case '"':
          *d++ = '\"';
          break;
        case '0':
        case '1':
        case '2':
        case '3':  // octal digit: 1 to 3 digits
        case '4':
        case '5':
        case '6':
        case '7': {
          const char* octal_start = p;
          unsigned int ch = *p - '0';
          if (p < last_byte && is_octal_digit(p[1])) ch = ch * 8 + *++p - '0';
          if (p < last_byte && is_octal_digit(p[1]))
            ch = ch * 8 + *++p - '0';  // now points at last digit
          if (ch > 0xff) {
            if (error) {
              *error = "Value of \\" +
                       string(octal_start, p + 1 - octal_start) +
                       " exceeds 0xff";
            }
            return false;
          }
          *d++ = ch;
          break;
        }
        case 'x':
        case 'X': {
          if (p >= last_byte) {
            if (error) *error = "String cannot end with \\x";
            return false;
          } else if (!ascii_isxdigit(p[1])) {
            if (error) *error = "\\x cannot be followed by a non-hex digit";
            return false;
          }
          unsigned int ch = 0;
          const char* hex_start = p;
          while (p < last_byte && ascii_isxdigit(p[1]))
            // Arbitrarily many hex digits
            ch = (ch << 4) + hex_digit_to_int(*++p);
          if (ch > 0xFF) {
            if (error) {
              *error = "Value of \\" + string(hex_start, p + 1 - hex_start) +
                       " exceeds 0xff";
            }
            return false;
          }
          *d++ = ch;
          break;
        }
        default: {
          if (error) *error = string("Unknown escape sequence: \\") + *p;
          return false;
        }
      }
      p++;  // read past letter we escaped
    }
  }
  *dest_len = d - dest;
  return true;
}

}  // namespace

bool CUnescape(StringPiece source, string* dest, string* error) {
  dest->resize(source.size());
  int dest_size;
  if (!CUnescapeInternal(source, const_cast<char*>(dest->data()), &dest_size,
                         error)) {
    return false;
  }
  dest->erase(dest_size);
  return true;
}

bool NumericParse32(const string& text, int32* val) {
  // Slow, but this code is not performance critical, and this
  // doesn't bring in any new dependencies
  char junk;
  if (sscanf(text.c_str(), "%d%c", val, &junk) == 1) {
    return true;
  } else {
    return false;
  }
}

void StripTrailingWhitespace(string* s) {
  string::size_type i;
  for (i = s->size(); i > 0 && isspace((*s)[i - 1]); --i) {
  }
  s->resize(i);
}

// Return lower-cased version of s.
string Lowercase(StringPiece s) {
  string result(s.data(), s.size());
  for (char& c : result) {
    c = tolower(c);
  }
  return result;
}

// Return upper-cased version of s.
string Uppercase(StringPiece s) {
  string result(s.data(), s.size());
  for (char& c : result) {
    c = toupper(c);
  }
  return result;
}

void TitlecaseString(string* s, StringPiece delimiters) {
  bool upper = true;
  for (string::iterator ss = s->begin(); ss != s->end(); ++ss) {
    if (upper) {
      *ss = toupper(*ss);
    }
    upper = (delimiters.find(*ss) != StringPiece::npos);
  }
}

size_t RemoveLeadingWhitespace(StringPiece* text) {
  size_t count = 0;
  const char* ptr = text->data();
  while (count < text->size() && isspace(*ptr)) {
    count++;
    ptr++;
  }
  text->remove_prefix(count);
  return count;
}

size_t RemoveTrailingWhitespace(StringPiece* text) {
  size_t count = 0;
  const char* ptr = text->data() + text->size() - 1;
  while (count < text->size() && isspace(*ptr)) {
    ++count;
    --ptr;
  }
  text->remove_suffix(count);
  return count;
}

size_t RemoveWhitespaceContext(StringPiece* text) {
  // use RemoveLeadingWhitespace() and RemoveTrailingWhitespace() to do the job
  return (RemoveLeadingWhitespace(text) + RemoveTrailingWhitespace(text));
}

bool ConsumePrefix(StringPiece* s, StringPiece expected) {
  if (s->starts_with(expected)) {
    s->remove_prefix(expected.size());
    return true;
  }
  return false;
}

bool ConsumeLeadingDigits(StringPiece* s, uint64* val) {
  const char* p = s->data();
  const char* limit = p + s->size();
  uint64 v = 0;
  while (p < limit) {
    const char c = *p;
    if (c < '0' || c > '9') break;
    uint64 new_v = (v * 10) + (c - '0');
    if (new_v < v) {
      // Overflow occurred
      return false;
    }
    v = new_v;
    p++;
  }
  if (p > s->data()) {
    // Consume some digits
    s->remove_prefix(p - s->data());
    *val = v;
    return true;
  } else {
    return false;
  }
}

bool SplitAndParseAsInts(StringPiece text, char delim,
                         std::vector<int32>* result) {
  result->clear();
  std::vector<string> num_strings = Split(text, delim);
  for (const auto& s : num_strings) {
    int32 num;
    if (!NumericParse32(s, &num)) return false;
    result->push_back(num);
  }
  return true;
}

}  // namespace str_util
}  // namespace tensorflow
