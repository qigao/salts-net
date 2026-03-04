# Documentation Restructuring

**Date**: 2024-03-04

This document describes the documentation restructuring for TurboScript to make it production-ready.

---

## Changes Made

### New Structure

```
docs/
├── README.md                    # Main navigation page (English)
├── getting-started.md           # 5-minute quick start
├── language-guide.md            # Complete syntax reference
├── api-reference.md             # Built-in functions reference
├── plugin-development.md        # Plugin development guide
├── advanced/                    # Advanced topics
│   └── architecture.md          # Internal architecture (merged from design.md + ARCHITECTURE.md)
├── modules/                     # Module-specific documentation
│   ├── README.md                # Module index
│   ├── FIN_MODULE.md            # Finance module
│   ├── PORTFOLIO.md             # Portfolio management
│   ├── FACTORS.md               # Factor analysis
│   ├── GRAPH_ALGORITHMS.md      # Graph algorithms
│   └── GRAPH_IMPLEMENTATION.md  # Graph implementation
├── zh/                          # Chinese translations
│   └── README.md                # Chinese main page
├── csv_filter_expression.md     # CSV filter syntax
├── math_cheatsheet.md           # Math functions cheatsheet
├── vec_cheatsheet.md            # Vector operations cheatsheet
├── ta_fin_cheatsheet.md         # TA/Finance cheatsheet
├── plugin_api_index.md          # Plugin API index
├── grammar.md                   # Legacy grammar reference (kept for reference)
├── ARCHITECTURE.md              # Legacy architecture doc (kept for reference)
└── PLUGIN_SYSTEM.md             # Legacy plugin doc (kept for reference)
```

### Files Created

1. **README.md** - New navigation page with clear structure
2. **getting-started.md** - 5-minute tutorial for new users
3. **language-guide.md** - Complete language reference (replaces grammar.md)
4. **api-reference.md** - Built-in functions reference
5. **plugin-development.md** - Simplified plugin guide (replaces PLUGIN_SYSTEM.md)
6. **advanced/architecture.md** - Merged design.md + ARCHITECTURE.md
7. **modules/README.md** - Module index
8. **zh/README.md** - Chinese translation of main page

### Files Deleted

1. **SESSION_SUMMARY.md** - Development notes (not needed in production docs)
2. **design.md** - Merged into advanced/architecture.md

### Files Moved

1. **FIN_MODULE.md** → `modules/FIN_MODULE.md`
2. **PORTFOLIO.md** → `modules/PORTFOLIO.md`
3. **FACTORS.md** → `modules/FACTORS.md`
4. **GRAPH_ALGORITHMS.md** → `modules/GRAPH_ALGORITHMS.md`
5. **GRAPH_IMPLEMENTATION.md** → `modules/GRAPH_IMPLEMENTATION.md`

### Files Kept (Legacy)

These files are kept for backward compatibility but should be considered deprecated:

1. **grammar.md** - Use `language-guide.md` instead
2. **ARCHITECTURE.md** - Use `advanced/architecture.md` instead
3. **PLUGIN_SYSTEM.md** - Use `plugin-development.md` instead

---

## Rationale

### Problems with Old Structure

1. **No clear entry point** - Users didn't know where to start
2. **Mixed audiences** - User docs and developer docs mixed together
3. **Language inconsistency** - Chinese and English mixed
4. **No quick start** - Steep learning curve for new users
5. **Poor organization** - Module docs scattered in root directory

### New Structure Benefits

1. **Clear navigation** - README.md as single entry point
2. **Audience separation** - Users vs Developers vs Contributors
3. **Language separation** - English primary, Chinese in zh/
4. **Quick start** - 5-minute tutorial for immediate productivity
5. **Better organization** - Modules in modules/, advanced topics in advanced/

---

## Migration Guide

### For External Links

If you have external links to old documentation:

| Old Path | New Path |
|----------|----------|
| `docs/grammar.md` | `docs/language-guide.md` |
| `docs/PLUGIN_SYSTEM.md` | `docs/plugin-development.md` |
| `docs/ARCHITECTURE.md` | `docs/advanced/architecture.md` |
| `docs/FIN_MODULE.md` | `docs/modules/FIN_MODULE.md` |
| `docs/PORTFOLIO.md` | `docs/modules/PORTFOLIO.md` |

### For Internal References

All internal links in the new documentation have been updated to reflect the new structure.

---

## Next Steps

### Recommended Improvements

1. **Complete Chinese translations** - Translate getting-started.md, language-guide.md, etc.
2. **Add more examples** - Real-world use cases in each module
3. **API documentation generation** - Auto-generate API docs from source code
4. **Video tutorials** - Screen recordings for visual learners
5. **Interactive playground** - Web-based REPL for trying TurboScript

### Maintenance

- Keep English docs as primary source of truth
- Update Chinese translations when English docs change
- Deprecate and eventually remove legacy files (grammar.md, ARCHITECTURE.md, PLUGIN_SYSTEM.md)

---

## Feedback

If you have suggestions for improving the documentation structure, please open an issue or discussion on GitHub.

---

**Documentation restructured for production readiness**
