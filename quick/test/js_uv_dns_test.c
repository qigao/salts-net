#include "test_uv_fixture.h"
#include <stdint.h>

spec("js_uv_dns") {
    it("should resolve localhost") {
        JSTurboTestEnv env = {0};
        js_uv_test_env_init(&env);

        js_uv_test_eval(&env,
                        "var dnsResult = '';\n"
                        "try {\n"
                        "  dnsResult = turbo.dns.resolve('localhost');\n"
                        "} catch (err) {\n"
                        "  dnsResult = 'error';\n"
                        "}\n");
        
        JSValue prop = js_uv_test_global_prop(&env, "dnsResult");
        const char *res = JS_ToCString(env.ctx, prop);
        check_not_null(res);
        if (res) {
            check_str_ne(res, "error");
            check_size_gt(strlen(res), 0);
            JS_FreeCString(env.ctx, res);
        }
        JS_FreeValue(env.ctx, prop);
        
        js_uv_test_env_cleanup(&env);
    }
}
