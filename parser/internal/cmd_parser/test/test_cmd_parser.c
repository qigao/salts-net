#include <unity.h>
#include <cmd_arger.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_flag_parsing(void) {
    CmdArgerBool verbose = cmd_arger_false;
    CmdArgerBool help = cmd_arger_false;

    CmdArgerDesc optional_args[] = {
        cmd_arger_desc_flag(&verbose, "verbose", "Enable verbose output"),
        cmd_arger_desc_flag(&help, "help", "Show help"),
    };

    char* argv[] = {"test_app", "--verbose"};
    int argc = 2;

    cmd_arger_parse(optional_args, 2, NULL, 0, argc, argv, "test_app v1.0", cmd_arger_false);

    TEST_ASSERT_TRUE(verbose);
    TEST_ASSERT_FALSE(help);
}

void test_string_parsing(void) {
    char* name = NULL;
    char* mode = "default";

    CmdArgerDesc optional_args[] = {
        cmd_arger_desc_string(&mode, "mode", "Set mode"),
    };
    CmdArgerDesc required_args[] = {
        cmd_arger_desc_string(&name, "name", "Name argument"),
    };

    char* argv[] = {"test_app", "--mode", "fast", "myname"};
    int argc = 4;

    cmd_arger_parse(optional_args, 1, required_args, 1, argc, argv, "test_app v1.0", cmd_arger_false);

    TEST_ASSERT_EQUAL_STRING("fast", mode);
    TEST_ASSERT_EQUAL_STRING("myname", name);
}

void test_short_flag_parsing(void) {
    CmdArgerBool verbose = cmd_arger_false;
    char* output = NULL;

    CmdArgerDesc optional_args[] = {
        cmd_arger_desc_flag_sh(&verbose, "verbose", "v", "Enable verbose"),
        cmd_arger_desc_string_sh(&output, "output", "o", "Output file"),
    };

    char* argv[] = {"test_app", "-v", "-o", "result.txt"};
    int argc = 4;

    cmd_arger_parse(optional_args, 2, NULL, 0, argc, argv, "test_app v1.0", cmd_arger_false);

    TEST_ASSERT_TRUE(verbose);
    TEST_ASSERT_EQUAL_STRING("result.txt", output);
}

void test_subcommand_parsing(void) {
    // Global options
    CmdArgerBool verbose = cmd_arger_false;
    CmdArgerDesc global_opts[] = {
        cmd_arger_desc_flag(&verbose, "verbose", "Enable verbose output"),
    };

    // Subcommand: commit
    char* msg = NULL;
    CmdArgerDesc commit_opts[] = {
        cmd_arger_desc_string(&msg, "message", "Commit message"),
    };
    CmdArgerSubCommand subcommands[] = {
        {
            .name = "commit",
            .info = "Commit changes",
            .optional_args = commit_opts,
            .optional_args_count = 1,
            .required_args = NULL,
            .required_args_count = 0
        }
    };

    // Case 1: Select subcommand
    // Note: The parser expects the executable name as argv[0], so "app" is 0.
    // Argument indices start from 1.
    // So: argv[0]="app", argv[1]="--verbose", argv[2]="commit", argv[3]="--message", argv[4]="hello"
    char* argv1[] = {"app", "--verbose", "commit", "--message", "hello"};
    int argc1 = 5;
    int selected = -1;

    cmd_arger_parse_subcommand(global_opts, 1, subcommands, 1, &selected, argc1, argv1, "app v1", cmd_arger_false);

    TEST_ASSERT_TRUE(verbose);
    TEST_ASSERT_EQUAL_INT(0, selected);
    TEST_ASSERT_EQUAL_STRING("hello", msg);
}

void test_env_parsing(void) {
    CmdArgerBool debug = cmd_arger_false;
    char* api_key = "default";
    int64_t retries = 0;

    CmdArgerDesc opts[] = {
        cmd_arger_with_env(cmd_arger_desc_flag(&debug, "debug", "Debug mode"), "TEST_APP_DEBUG"),
        cmd_arger_with_env(cmd_arger_desc_string(&api_key, "key", "API Key"), "TEST_APP_KEY"),
        cmd_arger_with_env(cmd_arger_desc_integer(&retries, "retries", "Retry count"), "TEST_APP_RETRIES"),
    };

    // Set env vars
    // Note: putenv is not portable or safe, use _putenv_s on Windows or setenv on POSIX.
    // Unity tests usually run on host.
    #ifdef _WIN32
    _putenv("TEST_APP_DEBUG=true");
    _putenv("TEST_APP_KEY=secret");
    _putenv("TEST_APP_RETRIES=5");
    #else
    setenv("TEST_APP_DEBUG", "true", 1);
    setenv("TEST_APP_KEY", "secret", 1);
    setenv("TEST_APP_RETRIES", "5", 1);
    #endif

    // Parse empty argv
    char* argv[] = {"app"};
    cmd_arger_parse(opts, 3, NULL, 0, 1, argv, "app", cmd_arger_false);

    TEST_ASSERT_TRUE(debug);
    TEST_ASSERT_EQUAL_STRING("secret", api_key);
    TEST_ASSERT_EQUAL_INT64(5, retries);

    // Test override by CLI
    #ifdef _WIN32
    _putenv("TEST_APP_DEBUG=false");
    #else
    setenv("TEST_APP_DEBUG", "false", 1);
    #endif
    
    char* argv2[] = {"app", "--debug"};
    cmd_arger_parse(opts, 3, NULL, 0, 2, argv2, "app", cmd_arger_false);
    TEST_ASSERT_TRUE(debug); // CLI overrides Env (env says false, cli says true)
}

void test_option_grouping(void) {
   // Just ensuring API compiles and runs without crashing.
   // Visual verification is manual, but logic coverage is valuable.
    char* input = NULL;
    char* output = NULL;
    
    CmdArgerDesc opts[] = {
        cmd_arger_with_group(cmd_arger_desc_string(&input, "input", "Input file"), "IO Options"),
        cmd_arger_with_group(cmd_arger_desc_string(&output, "output", "Output file"), "IO Options"),
        cmd_arger_desc_flag(NULL, "verbose", "Verbose mode"), // Ungrouped
    };
    
    // Normal parsing
    char* argv[] = {"app", "--input", "in.txt"};
    cmd_arger_parse(opts, 3, NULL, 0, 3, argv, "app", cmd_arger_false);
    TEST_ASSERT_EQUAL_STRING("in.txt", input);
}

void test_string_choices(void) {
    char* method = "GET";
    const char* choices[] = {"GET", "POST", "DELETE"};
    
    CmdArgerDesc opts[] = {
        cmd_arger_with_choices(cmd_arger_desc_string(&method, "method", "HTTP Method"), choices, 3),
    };
    
    // Valid
    char* argv1[] = {"app", "--method", "POST"};
    cmd_arger_parse(opts, 1, NULL, 0, 3, argv1, "app", cmd_arger_false);
    TEST_ASSERT_EQUAL_STRING("POST", method);
    
    TEST_ASSERT_EQUAL_STRING("POST", method);
    
    // Invalid should exit(1) but we can't test exit easily in simple unit tests without fork/longjmp.
    // For now, testing positive case and API presence.
}

static CmdArgerBool validate_port(const char* value, const char** error_message) {
    char* end;
    long v = strtol(value, &end, 10);
    if (*end != '\0') {
        *error_message = "must be an integer";
        return cmd_arger_false;
    }
    if (v < 1024 || v > 65535) {
        *error_message = "must be between 1024 and 65535";
        return cmd_arger_false;
    }
    return cmd_arger_true;
}

void test_custom_validator(void) {
    int64_t port = 0;
    CmdArgerDesc opts[] = {
        cmd_arger_with_validator(cmd_arger_desc_integer(&port, "port", "Port number"), validate_port),
    };
    
    // Valid
    char* argv1[] = {"app", "--port", "8080"};
    cmd_arger_parse(opts, 1, NULL, 0, 3, argv1, "app", cmd_arger_false);
    TEST_ASSERT_EQUAL_INT64(8080, port);
    
    // Invalid logic tested manually (will exit)
}

 
void test_response_file(void) {
    char* mode = "default";
    CmdArgerDesc opts[] = {
        cmd_arger_desc_string(&mode, "mode", "Mode"),
    };
    
    // Create temporary file
    const char* filename = "args.txt";
    FILE* f = fopen(filename, "wb");
    if (f) {
        fprintf(f, "--mode fast");
        fclose(f);
    }
    
    char* argv[] = {"app", "@args.txt"};
    cmd_arger_parse(opts, 1, NULL, 0, 2, argv, "app", cmd_arger_false);
    
    TEST_ASSERT_EQUAL_STRING("fast", mode);
    
    remove(filename);
}

void test_dotenv_integration(void) {
    char* api_url = "default";
    CmdArgerDesc opts[] = {
        cmd_arger_with_env(cmd_arger_desc_string(&api_url, "url", "API URL"), "TEST_API_URL"),
    };

    // Create .env file
    FILE* f = fopen(".env", "wb");
    if (f) {
        fprintf(f, "TEST_API_URL=https://api.example.com\n");
        fclose(f);
    }

    char* argv[] = {"app"};
    cmd_arger_parse(opts, 1, NULL, 0, 1, argv, "app", cmd_arger_false);

    TEST_ASSERT_EQUAL_STRING("https://api.example.com", api_url);

    remove(".env");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_flag_parsing);
    RUN_TEST(test_string_parsing);
    RUN_TEST(test_short_flag_parsing);
    RUN_TEST(test_subcommand_parsing);
    RUN_TEST(test_env_parsing);
    RUN_TEST(test_option_grouping);
    RUN_TEST(test_string_choices);
    RUN_TEST(test_custom_validator);
    RUN_TEST(test_response_file);
    RUN_TEST(test_dotenv_integration);
    return UNITY_END();
}
