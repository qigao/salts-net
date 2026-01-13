// http_example.js - Updated for TurboNet modules
async function runDemo() {
    console.log("=== TurboNet HTTP Client JS Demo (Enhanced) ===");

    try {
        // 1. GET Request with custom headers
        console.log("\n[1] Fetching with custom headers...");
        const todo = turbo.http.get("https://jsonplaceholder.typicode.com/todos/1", {
            headers: {
                "X-Client-Name": "TurboNetAgent",
                "Accept": "application/json"
            }
        });
        console.log("Status:", todo.status);
        console.log("Title:", todo.json?.title);

        // 2. POST Request with Auto-JSON
        console.log("\n[2] Creating a new post (Automatic Content-Type: application/json)...");
        const newPost = {
            title: "TurboNet Agent",
            body: "The module now auto-sets JSON headers for objects.",
            userId: 1337
        };
        const response = turbo.http.post("https://jsonplaceholder.typicode.com/posts", newPost);
        console.log("Status:", response.status);
        console.log("Response JSON Title:", response.json?.title);
        console.log("Response JSON Body: ", response.json?.body);

        // 3. Generic Request (DELETE)
        console.log("\n[3] Testing Generic Request (DELETE)...");
        const deleteRes = turbo.http.request("DELETE", "https://jsonplaceholder.typicode.com/posts/1");
        console.log("Delete Status:", deleteRes.status);

        // 4. File System Operations
        console.log("\n[4] Testing File System Operations...");
        const testData = { message: "Hello from TurboNet!", timestamp: Date.now() };
        turbo.fs.writeJson("test.json", testData);
        const readData = turbo.fs.readJson("test.json");
        console.log("Read back:", readData.message);

        // 5. DNS Resolution
        console.log("\n[5] Testing DNS Resolution...");
        try {
            const ip = turbo.dns.resolve("google.com");
            console.log("Google IP:", ip);
        } catch (err) {
            console.log("DNS resolution failed:", err.message);
        }

    } catch (err) {
        console.log("Demo Error:", err);
    }
}

runDemo();