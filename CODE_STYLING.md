# Code style

- Use four spaces, no tabs, and aim for at most 120 columns.
- Leave a blank line between declaration groups and logical steps. Keep related
  statements together; do not compress separate operations onto one line.
- Put function opening braces on the next line; put control-block and struct
  opening braces on the same line. Always brace control blocks and expand their bodies.
- Use `Type* pointer`, spaces around operators, and `if (condition)`.
- Use `snake_case` for files and variables, `camelCase` for functions,
  `PascalCase` for structure names, and `UPPER_SNAKE_CASE` for constants.
  Preserve Amiga API names, layouts and register conventions.
- Keep private headers in `include/internal/` and public graphics headers in
  `include/graphics/`. Use `#pragma once` for new headers.
- Preserve include order, especially macros before SDK headers. Explain intent
  and compatibility requirements in comments rather than narrating the code.
- Keep changes focused. Do not reformat unrelated code or generated/vendor files.

Use clang-format 18 with the supplied `.clang-format` for changed C files and
headers. Review its output, especially register arguments and inline assembly;
blank lines between logical steps and missing braces still need human review.

```sh
clang-format -i src/changed_file.c include/internal/changed_file.h
```

Retain SPDX identifiers, copyright notices and attribution. New code uses
MPL-2.0; reused code must have compatible licensing and documented provenance.
Do not import GPL code.
