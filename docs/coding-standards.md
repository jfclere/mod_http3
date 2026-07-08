# Coding Standards for mod_http3

This document defines the coding style, formatting rules, and documentation standards for `mod_http3`. Adhering to a single style ensures that the codebase remains readable, consistent, and maintainable.

These standards are based on the Apache Developers' C Language Style Guide, specifically resolved to match the project's C layout and active `.clang-format` configuration.

---

## 1. Indentation and Tab Size

- **Indent Size**: We use **four (4) spaces** per indentation level.
- **No Tabs**: Do not use tab characters in any source files. Indentation must consist entirely of spaces.
- **Clang-Format Enforcement**:
  ```yaml
  IndentWidth: 4
  TabWidth: 4
  UseTab: Never
  ```

---

## 2. Line Length and Wrapping

- **Standard Length**: Source code lines should ideally be kept under **80 characters** wide where practical, to fit comfortably in standard terminal editors and side-by-side diff views.
- **Enforced Limit**: The active `.clang-format` configuration sets `ColumnLimit: 300`; the formatter only force-wraps lines beyond that. The 80-character guidance is aspirational and applied at the author's discretion (long log calls and function signatures commonly exceed it).
- **Line Wrapping**: When a statement or function declaration exceeds the length limit, split it at a logical point.
- **Indentation of Wrapped Lines**:
  - The wrapped portion should be aligned under the first term of the expression or first parameter of the function call/declaration:
    ```c
    static const char* really_long_name(int i, int j,
                                        const char* args, void* foo,
                                        int k);
    ```
  - For long strings, split them into multiple lines. Compilers will automatically glue adjacent string literals:
    ```c
    printf("This is an example of how we split lines longer than %d characters\n"
           "into several so that they won't exceed this limit.\n",
           max_sourcecode_width);
    ```
  - For conditionals, keep sub-expressions atomic and place boolean operators at the start of wrapped lines:
    ```c
    if (cond1 && (item2 || item3) && (!item4)
        && (item5 || item6) && item7)
    {
        do_a_thing();
    }
    ```

---

## 3. Bracket Placement

We use a **symmetric bracket placement** style. The opening and closing braces are aligned on their own lines.

### Control Statements
Opening braces are placed on a new line directly below the control statement, and closing braces are placed on a separate line at the same indentation level:
```c
if (something)
{
    do_work();
}
else
{
    do_other_work();
}
```

### Switch Statements
- Case keywords are indented at the same level as the `switch` statement itself.
- Code blocks under a case are indented by 4 spaces.
- If a case block contains local variables, wrap the block in braces.
```c
switch (x)
{
case 1:
    printf("X is one!\n");
    break;
case 2:
{
    int y = x * 2;
    printf("X is two, double is %d!\n", y);
    break;
}
}
```

### Single Statement Blocks
Use brackets around a single statement:
```c
if (condition)
{
    execute_single_action();
}
```

---

## 4. Spacing

- **Control Keywords**: Place exactly one space before the opening parenthesis of control keywords (e.g., `if (expr)`, `for (expr)`, `while (expr)`, `switch (expr)`). Do NOT place spaces immediately inside the parentheses (e.g., `if ( expr )` is incorrect).
- **Functions**: Do not place a space between the function name and the opening parenthesis of arguments (e.g., `func(a, b)` instead of `func (a, b)`).
- **Lists and Loops**: Place a single space after commas in argument lists and after semicolons in `for` statements.
- **Operators**: Surround binary operators with spaces (e.g., `a = b`, `a + b`, `a < b`). Do not place spaces between unary operators and their operands (e.g., `++i`, `--i`, `!flag`, `-value`).

---

## 5. Pointer Syntax and Alignment

To align with modern conventions and our `.clang-format` configuration, pointers are left-aligned to the type rather than the variable name:

- **Type Declarations**: `h3_stream* stream` (NOT `h3_stream *stream` or `h3_stream * stream`).
- **Pointers to Pointers**: `char** argv`.
- **Casts**: Do not place spaces between a cast and the modified item, and keep the pointer alignment left-aligned:
  ```c
  char* copy = apr_pstrndup(stream->pool, (const char*)value->base, value->len);
  ```

---

## 6. Code Documentation (Doxygen)

We use Doxygen-style comments to generate automatic API documentation.

### One-Line Comments
For single-line descriptions of variables, members, or helper functions, use three slashes (`///`):
```c
/// The active configuration context for this connection
h3_config* cfg;
```

### Multi-Line Comments
For function interfaces, classes, and complex structures, use block comments beginning with `/**` and with each line prefixed by ` *`. Document parameters with `@param` and the return value with `@return`, aligning descriptions into a readable column:
```c
/**
 * ALPN selection callback for the QUIC SSL_CTX. Negotiates "h3" as the
 * single supported protocol.
 * @param ssl     The SSL object performing the negotiation.
 * @param out     Out: pointer to the selected protocol bytes.
 * @param outlen  Out: length of the selected protocol.
 * @param in      Wire-format ALPN extension from the peer.
 * @param inlen   Length of @p in.
 * @param arg     User data pointer (unused).
 * @return SSL_TLSEXT_ERR_OK if h3 was successfully negotiated, or
 *         SSL_TLSEXT_ERR_ALERT_FATAL if the client did not offer h3.
 */
int h3_alpn_select_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                      const unsigned char* in, unsigned int inlen, void* arg);
```
Refer to other parameters within descriptions using `@p name`.

### Internal/Developer Comments
Use normal double-slash (`//`) or block (`/* ... */`) comments for internal explanations, debugging notes, or TODOs that should not be included in the public API documentation.
