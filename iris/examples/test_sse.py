import requests
import datetime
import sys

def test_sse():
    url = "http://localhost:8080/stream"
    print(f"Connecting to {url}...")
    
    try:
        # stream=True is crucial here to prevent requests from buffering the whole response
        with requests.get(url, stream=True) as r:
            print(f"Status: {r.status_code}")
            print("Headers:")
            for k, v in r.headers.items():
                print(f"  {k}: {v}")
            print("-" * 40)
            print("Listening for events...")
            
            # Iterate over lines as they arrive
            for line in r.iter_lines():
                if line:
                    decoded = line.decode('utf-8')
                    timestamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    print(f"[{timestamp}] {decoded}")
                    
    except KeyboardInterrupt:
        print("\nStopped.")
    except Exception as e:
        print(f"Error: {e}")
        print("\nMake sure the sse_example.exe server is running on port 8080!")

if __name__ == "__main__":
    test_sse()
