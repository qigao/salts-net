# turl Examples

`turl` is a powerful curl-like command line utility built using `TurboNet::HttpClient`. It supports standard HTTP operations and features advanced **Mustache templating** via JSON context, **environment variable integration**, **response files**, and **grouped help output**.

## Basic Usage

### Simple GET Request

```bash
turl https://httpbin.org/get
```

### POST Data

```bash
turl --request POST --data "name=TurboNet&type=Framework" https://httpbin.org/post
```

### Short Options

Most common options have short aliases for convenience:

```bash
turl -v -L -X POST -d "data" -H "Content-Type: application/json" https://httpbin.org/post
```

### Short Flags Reference

Use short flags to write more concise commands:

| Short Flag | Long Flag | Description |
|------------|-----------|-------------|
| `-X` | `--request` | Specify request method (GET, POST, PUT, DELETE, etc.) |
| `-d` | `--data` | HTTP POST data (supports Mustache templating) |
| `-H` | `--header` | Pass custom header(s) to server |
| `-F` | `--form` | Specify multipart MIME data |
| `-u` | `--user` | Server user and password for basic auth |
| `-o` | `--output` | Write to file instead of stdout |
| `-v` | `--verbose` | Make the operation more talkative |
| `-L` | `--location` | Follow redirects |
| `-j` | `--context` | JSON context for Mustache templates |
| `-t` | `--test` | Post-request JavaScript test script |

**Example: Simplified Login Request**

```bash
# Long form
turl --request POST --data "{\"u\": \"admin\"}" --test save_session.js https://api.com/login

# Short form (equivalent)
turl -X POST -d "{\"u\": \"admin\"}" -t save_session.js https://api.com/login
```

### Sending Binary Data (Protobuf / FlatBuffers)

Use `--data-binary` to send raw binary files. This bypasses Mustache templating to prevent data corruption and supports null bytes.

```bash
# Send a compiled FlatBuffer or Protobuf file
turl --data-binary @data.pb https://api.example.com/v1/log
```

### Custom Headers

```bash
turl --header "Accept: application/json" --header "X-Custom: Hello" https://httpbin.org/headers
```

### Verbose Mode & Follow Redirects

```bash
turl --verbose --location https://google.com
```

---

## Environment Variables

`turl` automatically reads environment variables for common options, making it easier to configure defaults without command-line flags.

### Supported Environment Variables

| Environment Variable | Option | Description |
|---------------------|--------|-------------|
| `TURL_VERBOSE` | `--verbose` | Enable verbose output (set to `true`, `1`, `yes`, or `on`) |
| `TURL_USER` | `--user` | Default basic auth credentials |
| `TURL_BEARER_TOKEN` | `--bearer` | Default bearer token for authentication |
| `TURL_CONTEXT` | `--context` | Default JSON context file for templating |

### Example: Set Default Authentication

```bash
# Set environment variable
export TURL_BEARER_TOKEN="your_secret_token"

# No need to specify --bearer anymore
turl https://api.example.com/protected
```

### Example: Enable Verbose Mode Globally

```bash
export TURL_VERBOSE=true
turl https://httpbin.org/get  # Will be verbose automatically
```

**Note:** Command-line flags always override environment variables.

---

## Response Files

`turl` supports GCC/Clang-style response files using the `@filename` syntax. This is useful for managing complex command lines or avoiding shell escaping issues.

### Example: Arguments in a File

Create a file named `request.txt`:

```
--verbose
--request POST
--header "Content-Type: application/json"
--data {"name":"TurboNet"}
https://httpbin.org/post
```

Run it with:

```bash
turl @request.txt
```

### Combined Usage

You can mix response files with regular arguments:

```bash
turl @common-headers.txt --bearer "custom_token" https://api.example.com/v1/data
```

---

## Mustache Templating

`turl` allows you to use `{{variable}}` syntax in the URL, headers, and body by providing a JSON context with the `-j` (or `--context`) flag.

### 1. Templated URL

Use variables to construct your URL dynamically.

```bash
turl --context "{\"base\": \"httpbin.org\", \"endpoint\": \"get\"}" "https://{{base}}/{{endpoint}}"
```

### 2. Templated Body

Inject data into your POST body.

```bash
turl --request POST --context "{\"user\": \"Antigravity\", \"version\": \"1.0\"}" --data "{\"name\": \"{{user}}\", \"v\": \"{{version}}\"}" https://httpbin.org/post
```

### 3. Templated Headers

Manage authentication or custom headers with templates.

```bash
turl --context "{\"token\": \"secret_123\"}" --header "Authorization: Bearer {{token}}" https://httpbin.org/headers
```

### 4. Using a JSON File for Context

If you have a file named `context.json`:

```json
{
  "api_key": "my-secret-key",
  "search": "turbonet",
  "limit": 10
}
```

You can run:

```bash
turl --context context.json --header "X-API-Key: {{api_key}}" "https://httpbin.org/get?q={{search}}&l={{limit}}"
```

---

## Advanced Authentication

### Basic Authentication

```bash
turl --user "user:password" https://httpbin.org/basic-auth/user/password
```

### Bearer Token

```bash
turl --bearer "your_token_here" https://httpbin.org/bearer
```

### Environment-Based Auth

```bash
export TURL_BEARER_TOKEN="persistent_token"
turl https://api.example.com/me
```

### Sending Form Data (Multipart)

Use `--form` to send multipart form data, which is essential for file uploads.

```bash
# Send text fields and a file
turl --form "username=john" --form "avatar=@profile.png" https://api.example.com/upload
```

### WebSocket Support

`turl` supports WebSocket out of the box when you use the `ws://` or `wss://` scheme.

```bash
# Connect and send a message
turl --verbose --data "Hello WebSocket" ws://echo.websocket.org

# Use secure WebSocket
turl wss://echo.websocket.org
```

When connecting to a WebSocket, `turl` will:

1. Perform the HTTP upgrade handshake.
2. Send the message provided via `-d` or `--data-binary` (if any).
3. Listen for incoming messages for up to 10 seconds.
4. Automatically **pretty-print** incoming messages if they are JSON.

## Advanced: JavaScript Scripting (Postman-style)

`turl` integrates `QuickJS` to allow pre-request scripts. You can use the `--script` flag to run a `.js` file that can modify the environment variables.

### Dynamic Variables (e.g., Timestamp, UUID)

If you have `pre-request.js`:

```javascript
// Generate a dynamic timestamp and a nonce
env.timestamp = Date.now();
env.nonce = Math.random().toString(36).substring(7);

console.log(`* Pre-request script: timestamp=${env.timestamp}, nonce=${env.nonce}`);
```

Run it with:

```bash
turl --script pre-request.js --data "{\"ts\": {{timestamp}}, \"nonce\": \"{{nonce}}\"}" https://httpbin.org/post
```

### Post-Response Scripting (Tests & Extraction)

The `--test` flag allows you to run a JavaScript script after the HTTP response has been received. This mimics Postman's "Tests" tab.

#### Available Globals in `--test`

* **`response`**:
  * `response.status`: (Number) Status code (e.g., 200).
  * `response.body`: (String) The raw response body.
  * `response.headers`: (Object) Key-value pairs of response headers.
  * `response.json()`: (Function) Helper that returns a parsed JSON object.
* **`env`**: The shared environment object (same as in `--script`).
* **`assert(condition, message)`**: Global helper for testing. Throws and prints an error if the condition is false.

#### Example: API Validation & Variable Capture

Create a file named `validate.js`:

```javascript
// 1. Check for success
assert(response.status === 200, "API returned error status");

// 2. Extract and verify data
let data = response.json();
assert(data.status === "success", "Response was not marked as success");

// 3. Log details (visible in CLI)
console.log(`* API Success: User ${data.user_id} logic confirmed.`);

// 4. Update the environment for future templating
env.cached_token = response.headers['X-Auth-Token'];
```

#### Request Chaining (Saving State)

You can persist the `env` object to a file using the `turbo.fs` module, allowing one `turl` run to provide data for the next.

`save_session.js`:

```javascript
// Extract token
env.token = response.json().auth_token;

// Save env back to context.json for the next command
turbo.fs.writeFileSync("context.json", JSON.stringify(env, null, 2));
console.log("* Session token saved to context.json");
```

Run first request:

```bash
turl -X POST -d "{\"u\": \"admin\"}" -t save_session.js https://api.com/login
```

Run second request (uses updated `context.json`):

```bash
turl -j context.json -H "Authorization: {{token}}" https://api.com/dashboard
```

### Scripting with `turbo` modules

Your scripts have access to the `turbo` global object (if enabled), allowing you to interact with the file system, DNS, more:

```javascript
// Read a secret from a local file
let secret = turbo.fs.readFileSync("secret.key");
env.auth_token = secret.trim();
```

---

## Help Output

`turl` features organized help output with **argument groups** for better readability:

```bash
turl --help
```

Output example:

```
------ turl 1.0 - turbonet HTTP/WebSocket client help ------
usage: turl [OPTIONS...] URL

ARGUMENTS:
 URL: The URL to request

OPTIONS:
 --help: this help screen

General Options:
 -v, --verbose: Make the operation more talkative
 -L, --location: Follow redirects

Request Options:
 -X, --request: Specify request method [GET|POST|PUT|DELETE|HEAD|PATCH|OPTIONS]
 -d, --data: HTTP POST data
 --data-binary: HTTP POST binary data (no templating)
 -H, --header: Pass custom header(s) to server
 -F, --form: Specify multipart MIME data

Authentication:
 -u, --user: Server user and password
 --bearer: Bearer token for authentication

Output Options:
 -o, --output: Write to file instead of stdout

Templating & Scripting:
 -j, --context: JSON context for mustache templates
 --env: Alias for --context
 --script: Pre-request JavaScript script
 -t, --test: Post-request JavaScript test script

WebSocket Options:
 --ping: Send a WebSocket ping
```

---

## Comparison: `turl` vs. `curl` vs. `Postman` vs. `Hoppscotch`

| Feature | `curl` | `Postman` | `Hoppscotch` | `turl` |
| :--- | :--- | :--- | :--- | :--- |
| **Type** | CLI (C) | Desktop (Electron) | Web / Desktop | **CLI (C)** |
| **Speed** | Ultra-Fast | Heavy | Medium | **Ultra-Fast** |
| **Automation** | Shell only | Newman (Node) | Hopper (Node) | **Native Binary** |
| **Scripting** | None | JS (Complex) | JS (Light) | **QuickJS (Embedded)** |
| **Templating** | None | Basic Var | Basic Var | **Full Mustache** |
| **Binary-Safe** | Yes | Partial | Partial | **Full (PB/FBS)** |
| **WS Support** | No | Yes | Yes | **Yes + Pretty** |
| **Env Vars** | Manual | Yes | Yes | **Automatic** |
| **Response Files** | No | No | No | **Yes (@file)** |
| **Grouped Help** | No | GUI | GUI | **Yes (CLI)** |

---

## Output Formatting

`turl` automatically detects JSON responses and **pretty-prints** them for better readability. It also supports ANSI colors for verbose mode.

### Pretty JSON

```bash
turl https://httpbin.org/json
```

### JSON in Verbose Mode

```bash
turl --verbose https://httpbin.org/get
```

* **Blue text**: Shows the request attempt.
* **Yellow text**: Shows the response headers.
* **Green text**: Shows the status code.

---

## Debugging

Use the `--verbose` flag to see request details and response headers:

```bash
turl --verbose https://httpbin.org/ip
```

Output:

```text
* Trying to GET https://httpbin.org/ip...
< HTTP/1.1 200
Date: Wed, 21 Jan 2026 05:08:00 GMT
Content-Type: application/json
...
{
  "origin": "1.2.3.4"
}
```
