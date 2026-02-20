#include "dsv_filter.h"
#include "csv_parser.h"
#include "tinytest.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void my_callback(void *user_data, size_t row_index, const char *rendered_row) {
    char *buffer = (char*)user_data;
    strcat(buffer, rendered_row);
    strcat(buffer, "\n");
}

spec("dsv_filter") {
    describe("Basic Filtering") {
        it("should filter rows based on number column") {
            const char *csv_data = 
                "name,age_n,role\n"
                "Alice,30,dev\n"
                "Bob,40,manager\n"
                "Charlie,25,intern\n";
            
            csv_doc_t *doc = csv_parse(csv_data, strlen(csv_data));
            check_not_null(doc);
            
            dsv_filter_t *filter = dsv_filter_create(doc, 0);
            check_not_null(filter);
            
            bool compiled = dsv_filter_compile(filter, "age > 30"); // age_n -> age
            check(compiled);
            if (!compiled) {
                // If we could access error, but check() macro aborts on failure mostly or continues?
                // tinytest check() usually continues.
                printf("Filter error: %s\n", dsv_filter_error(filter));
            }
            
            char buffer[1024] = {0};
            dsv_filter_run(filter, my_callback, buffer);
            
            // Expected: Only Bob (age 40)
            check_str_eq(buffer, "Bob|40|manager\n");
            
            dsv_filter_destroy(filter);
            csv_free(doc);
        }
        
        it("should filter rows based on string column") {
            const char *csv_data = 
                "name_s,score_n\n"
                "Alice,10\n"
                "Bob,5\n";
            
            csv_doc_t *doc = csv_parse(csv_data, strlen(csv_data));
            check_not_null(doc);
            
            dsv_filter_t *filter = dsv_filter_create(doc, 0);
            check_not_null(filter);
            
            // name_s -> name
            bool compiled = dsv_filter_compile(filter, "name == \"Alice\"");
            check(compiled);
            
            char buffer[1024] = {0};
            dsv_filter_run(filter, my_callback, buffer);
            
            check_str_eq(buffer, "Alice|10\n");
            
            dsv_filter_destroy(filter);
            csv_free(doc);
        }

        it("should handle custom output delimiter") {
             const char *csv_data = "a_n,b_n\n1,2\n3,4\n";
             csv_doc_t *doc = csv_parse(csv_data, strlen(csv_data));
             dsv_filter_t *filter = dsv_filter_create(doc, 0);
             dsv_filter_set_output_delimiter(filter, ';');
             
             bool compiled = dsv_filter_compile(filter, "a + b == 3"); // 1+2=3 matches
             check(compiled);
             
             char buffer[1024] = {0};
             dsv_filter_run(filter, my_callback, buffer);
             
             check_str_eq(buffer, "1;2\n");
             
             dsv_filter_destroy(filter);
             csv_free(doc);
        }
    }
}
