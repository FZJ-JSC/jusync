#!/usr/bin/env python3
"""
ZeroMQ DEALER client for sending USD files and images to ANARI USD Middleware
Compatible with the ROUTER socket pattern used in your test_middleware application
"""

import zmq
import hashlib
import os
import sys
import time
import json
from pathlib import Path

class ANARIUSDClient:
    def __init__(self, endpoint="tcp://localhost:5556"):
        self.endpoint = endpoint
        self.context = zmq.Context()
        self.socket = None
        self.connected = False
        
    def connect(self):
        """Connect to the ANARI USD Middleware server"""
        try:
            self.socket = self.context.socket(zmq.DEALER)
            # Set socket options
            self.socket.setsockopt(zmq.LINGER, 1000)  # 1 second linger
            self.socket.setsockopt(zmq.RCVTIMEO, 5000)  # 5 second receive timeout
            self.socket.setsockopt(zmq.SNDTIMEO, 5000)  # 5 second send timeout
            
            # Set identity for debugging (optional)
            identity = f"client_{os.getpid()}_{int(time.time())}"
            self.socket.setsockopt_string(zmq.IDENTITY, identity)
            
            print(f"🔌 Connecting to {self.endpoint}...")
            self.socket.connect(self.endpoint)
            self.connected = True
            print(f"✅ Connected successfully as {identity}")
            return True
            
        except Exception as e:
            print(f"❌ Connection failed: {e}")
            return False
    
    def disconnect(self):
        """Disconnect from the server"""
        if self.socket:
            self.socket.close()
        if self.context:
            self.context.term()
        self.connected = False
        print("🔌 Disconnected")
    
    def calculate_sha256(self, data):
        """Calculate SHA256 hash of data"""
        return hashlib.sha256(data).hexdigest()
    
    def send_file(self, file_path):
        """
        Send a file to the middleware server
        Expected message format: [Filename] [Content] [Hash]
        """
        if not self.connected:
            print("❌ Not connected to server")
            return False
            
        try:
            # Read file
            file_path = Path(file_path)
            if not file_path.exists():
                print(f"❌ File not found: {file_path}")
                return False
                
            with open(file_path, 'rb') as f:
                file_data = f.read()
            
            # Calculate hash
            file_hash = self.calculate_sha256(file_data)
            filename = file_path.name
            
            print(f"\n📤 SENDING FILE")
            print(f"📄 Filename: {filename}")
            print(f"📊 Size: {self.format_bytes(len(file_data))}")
            print(f"🔐 Hash: {file_hash[:16]}...")
            
            # Send multi-part message: [Filename] [Content] [Hash]
            self.socket.send_string(filename, zmq.SNDMORE)
            self.socket.send(file_data, zmq.SNDMORE)
            self.socket.send_string(file_hash)
            
            print("📤 File sent, waiting for reply...")
            
            # Wait for reply
            try:
                reply = self.socket.recv_string()
                print(f"✅ Server reply: {reply}")
                return True
            except zmq.Again:
                print("⚠️ No reply received (timeout)")
                return False
                
        except Exception as e:
            print(f"❌ Error sending file: {e}")
            return False
    
    def send_message(self, message):
        """Send a simple text message"""
        if not self.connected:
            print("❌ Not connected to server")
            return False
            
        try:
            print(f"📤 Sending message: {message}")
            self.socket.send_string(message)
            
            # Wait for reply
            try:
                reply = self.socket.recv_string()
                print(f"✅ Server reply: {reply}")
                return True
            except zmq.Again:
                print("⚠️ No reply received (timeout)")
                return False
                
        except Exception as e:
            print(f"❌ Error sending message: {e}")
            return False
    
    def send_json_message(self, data):
        """Send a JSON message"""
        try:
            json_str = json.dumps(data, indent=2)
            return self.send_message(json_str)
        except Exception as e:
            print(f"❌ Error encoding JSON: {e}")
            return False
    
    def format_bytes(self, bytes_size):
        """Format bytes in human readable format"""
        for unit in ['B', 'KB', 'MB', 'GB']:
            if bytes_size < 1024.0:
                return f"{bytes_size:.2f} {unit}"
            bytes_size /= 1024.0
        return f"{bytes_size:.2f} TB"

def print_usage():
    """Print usage information"""
    print("""
🚀 ANARI USD Middleware ZMQ Client

Usage:
    python3 zmq_client.py [options] <command> [args...]

Options:
    --endpoint <addr>    ZMQ endpoint (default: tcp://localhost:5556)

Commands:
    send-file <path>     Send a USD file or image
    send-message <text>  Send a text message
    send-json <file>     Send JSON from file
    test                 Send test messages
    interactive          Interactive mode

Examples:
    python3 zmq_client.py send-file model.usd
    python3 zmq_client.py send-file texture.png
    python3 zmq_client.py --endpoint tcp://192.168.1.100:5556 send-file scene.usda
    python3 zmq_client.py send-message "Hello from Python client"
    python3 zmq_client.py test
    python3 zmq_client.py interactive
    """)

def run_test_sequence(client):
    """Run a sequence of test messages"""
    print("\n🧪 Running test sequence...")
    
    # Test 1: Simple message
    print("\n--- Test 1: Simple message ---")
    client.send_message("Hello from Python ZMQ client!")
    time.sleep(1)
    
    # Test 2: JSON message
    print("\n--- Test 2: JSON message ---")
    test_data = {
        "type": "test",
        "timestamp": int(time.time()),
        "client": "python_zmq_client",
        "data": {
            "message": "This is a test JSON message",
            "version": "1.0"
        }
    }
    client.send_json_message(test_data)
    time.sleep(1)
    
    # Test 3: Status request
    print("\n--- Test 3: Status request ---")
    client.send_message("STATUS")
    
    print("✅ Test sequence completed")

def interactive_mode(client):
    """Interactive mode for sending messages"""
    print("\n🎮 Interactive mode - Type 'quit' to exit")
    print("Commands: file <path>, message <text>, json <data>, quit")
    
    while True:
        try:
            user_input = input("\n> ").strip()
            if not user_input:
                continue
                
            if user_input.lower() == 'quit':
                break
                
            parts = user_input.split(' ', 1)
            command = parts[0].lower()
            
            if command == 'file' and len(parts) > 1:
                client.send_file(parts[1])
            elif command == 'message' and len(parts) > 1:
                client.send_message(parts[1])
            elif command == 'json' and len(parts) > 1:
                try:
                    json_data = json.loads(parts[1])
                    client.send_json_message(json_data)
                except json.JSONDecodeError:
                    print("❌ Invalid JSON format")
            else:
                print("❌ Unknown command. Use: file <path>, message <text>, json <data>, quit")
                
        except KeyboardInterrupt:
            break
        except EOFError:
            break
    
    print("\n👋 Exiting interactive mode")

def main():
    if len(sys.argv) < 2:
        print_usage()
        return 1
    
    # Parse arguments
    endpoint = "tcp://localhost:5556"
    args = sys.argv[1:]
    
    # Check for endpoint option
    if "--endpoint" in args:
        idx = args.index("--endpoint")
        if idx + 1 < len(args):
            endpoint = args[idx + 1]
            args = args[:idx] + args[idx + 2:]
        else:
            print("❌ --endpoint requires an address")
            return 1
    
    if not args:
        print_usage()
        return 1
    
    command = args[0]
    
    # Create and connect client
    client = ANARIUSDClient(endpoint)
    if not client.connect():
        return 1
    
    try:
        if command == "send-file":
            if len(args) < 2:
                print("❌ send-file requires a file path")
                return 1
            success = client.send_file(args[1])
            return 0 if success else 1
            
        elif command == "send-message":
            if len(args) < 2:
                print("❌ send-message requires a message")
                return 1
            message = " ".join(args[1:])
            success = client.send_message(message)
            return 0 if success else 1
            
        elif command == "send-json":
            if len(args) < 2:
                print("❌ send-json requires a JSON file path")
                return 1
            try:
                with open(args[1], 'r') as f:
                    json_data = json.load(f)
                success = client.send_json_message(json_data)
                return 0 if success else 1
            except Exception as e:
                print(f"❌ Error reading JSON file: {e}")
                return 1
                
        elif command == "test":
            run_test_sequence(client)
            return 0
            
        elif command == "interactive":
            interactive_mode(client)
            return 0
            
        else:
            print(f"❌ Unknown command: {command}")
            print_usage()
            return 1
            
    finally:
        client.disconnect()

if __name__ == "__main__":
    sys.exit(main())

