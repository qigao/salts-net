# Phase 10: External Library Integration

## 10.1 CSV Parser Integration
- [ ] Add `csv_parse(string)` to return a CSV document handle (or 2D vector if possible, but handle might be safer).
- [ ] Add `csv_get(doc, row, col)` to retrieve values.
- [ ] Add `csv_read_file(path)` as a convenience.

## 10.2 DateTime Integration
- [ ] Add `time()` to return current timestamp.
- [ ] Add `date(string)` to parse date string to timestamp.
- [ ] Add `format_date(timestamp, format)` to format timestamps.

## 10.3 File System Integration
- [ ] Add `read_file(path)` to read entire file into a string.
- [ ] Add `write_file(path, content)` to write string to file.
- [ ] Add `file_exists(path)`.

## 10.4 Build System Updates
- [ ] Update `build_exprtk.bat` or `CMakeLists.txt` to include:
    - `parser/internal/csv_parser/src/csv_parser.c`
    - `parser/internal/datetime_parser/src/datetime_parser.c`
    - `shared/utils/src/turbo_fs.c` (if available, or link against it)
- [ ] Ensure include paths are correct.

