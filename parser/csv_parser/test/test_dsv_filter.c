#include "dsv_filter.h"
#include "tinytest.h"
#include <string.h>

typedef struct {
    size_t count;
    size_t last_row;
    char last_row_text[256];
} run_ctx_t;

static void on_row(void *user_data, size_t row_index, const char *rendered_row) {
    run_ctx_t *ctx = (run_ctx_t *)user_data;
    ctx->count++;
    ctx->last_row = row_index;
    if (rendered_row) {
        size_t n = strlen(rendered_row);
        if (n >= sizeof(ctx->last_row_text)) n = sizeof(ctx->last_row_text) - 1;
        memcpy(ctx->last_row_text, rendered_row, n);
        ctx->last_row_text[n] = '\0';
    } else {
        ctx->last_row_text[0] = '\0';
    }
}

spec("dsv_filter") {
    describe("Compile and Evaluate") {
        it("should filter numeric expression") {
            const char *csv = "price_n,volume_n,sym_s\n"
                              "50,100,AAPL\n"
                              "150,200,GOOG\n"
                              "200,50,MSFT\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);
            check(dsv_filter_compile(f, "price > 100 and volume >= 100"));

            check_int_eq(dsv_filter_check_row(f, 1), 0);
            check_int_eq(dsv_filter_check_row(f, 2), 1);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should filter string expression") {
            const char *csv = "price_n,sym_s\n"
                              "100,AAPL\n"
                              "200,GOOG\n"
                              "150,AAPL\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);
            check(dsv_filter_compile(f, "sym == \"AAPL\""));

            check_int_eq(dsv_filter_check_row(f, 1), 1);
            check_int_eq(dsv_filter_check_row(f, 2), 0);
            check_int_eq(dsv_filter_check_row(f, 3), 1);

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should run callback for matched rows") {
            const char *csv = "price_n,volume_n,sym_s\n"
                              "50,100,AAPL\n"
                              "150,200,GOOG\n"
                              "250,300,MSFT\n";
            run_ctx_t ctx = {0};

            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);
            dsv_filter_set_output_delimiter(f, '|');
            check(dsv_filter_compile(f, "price >= 150"));

            dsv_filter_run(f, on_row, &ctx);

            check_int_eq(ctx.count, 2);
            check_int_eq(ctx.last_row, 3);
            check_str_eq(ctx.last_row_text, "250|300|MSFT");

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should filter with lhs arithmetic using two numeric columns") {
            const char *csv = "left_n,right_n,sym_s\n"
                              "2,3,A\n"
                              "4,6,B\n"
                              "7,1,C\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);
            check(dsv_filter_compile(f, "left + right >= 10"));

            check_int_eq(dsv_filter_check_row(f, 1), 0);
            check_int_eq(dsv_filter_check_row(f, 2), 1);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should filter with lhs arithmetic using numeric literal") {
            const char *csv = "value_n,sym_s\n"
                              "5,A\n"
                              "3,B\n"
                              "7,C\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);
            check(dsv_filter_compile(f, "value - 2 == 3"));

            check_int_eq(dsv_filter_check_row(f, 1), 1);
            check_int_eq(dsv_filter_check_row(f, 2), 0);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should filter with chained lhs arithmetic terms") {
            const char *csv = "a_n,b_n,c_n\n"
                              "1,2,3\n"
                              "4,1,0\n"
                              "5,2,1\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);
            check(dsv_filter_compile(f, "a + b - 1 + c == 5"));

            check_int_eq(dsv_filter_check_row(f, 1), 1);
            check_int_eq(dsv_filter_check_row(f, 2), 0);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should support unary sign before arithmetic column term") {
            const char *csv = "a_n,b_n\n"
                              "5,3\n"
                              "7,5\n"
                              "2,5\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);
            check(dsv_filter_compile(f, "a + -b == 2"));
            check_int_eq(dsv_filter_check_row(f, 1), 1);
            check_int_eq(dsv_filter_check_row(f, 2), 1);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            check(dsv_filter_compile(f, "a - -b == 12"));
            check_int_eq(dsv_filter_check_row(f, 1), 0);
            check_int_eq(dsv_filter_check_row(f, 2), 1);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should support multiplication and division precedence") {
            const char *csv = "a_n,b_n,c_n\n"
                              "2,3,4\n"
                              "6,2,4\n"
                              "9,3,1\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(dsv_filter_compile(f, "a + b * c == 14"));
            check_int_eq(dsv_filter_check_row(f, 1), 1);
            check_int_eq(dsv_filter_check_row(f, 2), 1);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            check(dsv_filter_compile(f, "a / b + c == 7"));
            check_int_eq(dsv_filter_check_row(f, 1), 0);
            check_int_eq(dsv_filter_check_row(f, 2), 1);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should support unary sign with multiplication term") {
            const char *csv = "a_n,b_n\n"
                              "2,3\n"
                              "3,3\n"
                              "2,2\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(dsv_filter_compile(f, "a * -b == -6"));
            check_int_eq(dsv_filter_check_row(f, 1), 1);
            check_int_eq(dsv_filter_check_row(f, 2), 0);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should support parentheses precedence in lhs arithmetic") {
            const char *csv = "a_n,b_n,c_n\n"
                              "2,3,4\n"
                              "4,1,2\n"
                              "3,2,1\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(dsv_filter_compile(f, "a * (b + c) == 14"));
            check_int_eq(dsv_filter_check_row(f, 1), 1);
            check_int_eq(dsv_filter_check_row(f, 2), 0);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            check(dsv_filter_compile(f, "a * (b + (c - 1)) == 8"));
            check_int_eq(dsv_filter_check_row(f, 1), 0);
            check_int_eq(dsv_filter_check_row(f, 2), 1);
            check_int_eq(dsv_filter_check_row(f, 3), 0);

            dsv_filter_destroy(f);
            csv_free(doc);
        }
    }

    describe("Error Handling") {
        it("should return -1 when row check is called before compile") {
            const char *csv = "x_n\n1\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check_int_eq(dsv_filter_check_row(f, 1), -1);

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail compile on unknown column") {
            const char *csv = "x_n\n1\n2\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, "missing > 1"));
            check(dsv_filter_error(f)[0] != '\0');

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail compile on empty expression") {
            const char *csv = "x_n\n1\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, ""));
            check(dsv_filter_error(f)[0] != '\0');

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail when arithmetic is used on string column") {
            const char *csv = "sym_s,val_n\nA,1\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, "sym + 1 == 2"));
            check_str_eq(dsv_filter_error(f), "invalid filter: arithmetic on string column");

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail when arithmetic rhs term is a string column") {
            const char *csv = "price_n,sym_s\n10,A\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, "price + sym > 10"));
            check_str_eq(dsv_filter_error(f), "invalid filter: arithmetic requires numeric columns");

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail when arithmetic compares with string value") {
            const char *csv = "price_n,sym_s\n10,A\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, "price + 1 == \"11\""));
            check_str_eq(dsv_filter_error(f), "invalid filter: arithmetic cannot compare to string");

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail when arithmetic rhs term is missing") {
            const char *csv = "price_n\n10\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, "price + == 10"));
            check_str_eq(dsv_filter_error(f), "invalid filter: expected column or number after +/-");

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail when a later arithmetic rhs term is missing") {
            const char *csv = "price_n\n10\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, "price + 1 - == 10"));
            check_str_eq(dsv_filter_error(f), "invalid filter: expected column or number after +/-");

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail when unary sign has no arithmetic term") {
            const char *csv = "price_n\n10\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, "price + - == 10"));
            check_str_eq(dsv_filter_error(f), "invalid filter: expected column or number after +/-");

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail when multiplication rhs term is missing") {
            const char *csv = "price_n\n10\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, "price * == 10"));
            check_str_eq(dsv_filter_error(f), "invalid filter: expected column or number after +/-");

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail when parentheses are unbalanced in lhs arithmetic") {
            const char *csv = "a_n,b_n\n1,2\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, "a * (b + 1 == 3"));
            check_str_eq(dsv_filter_error(f), "invalid filter: unbalanced parentheses in arithmetic expression");

            check(!dsv_filter_compile(f, "a * () == 0"));
            check_str_eq(dsv_filter_error(f), "invalid filter: empty parentheses in arithmetic expression");

            dsv_filter_destroy(f);
            csv_free(doc);
        }

        it("should fail when arithmetic expression has invalid character") {
            const char *csv = "a_n,b_n\n1,2\n";
            csv_doc_t *doc = csv_parse(csv, strlen(csv));
            check_not_null(doc);

            dsv_filter_t *f = dsv_filter_create(doc, 0);
            check_not_null(f);

            check(!dsv_filter_compile(f, "a * (b + @) == 0"));
            check_str_eq(dsv_filter_error(f), "invalid filter: invalid character in arithmetic expression");

            dsv_filter_destroy(f);
            csv_free(doc);
        }
    }
}
