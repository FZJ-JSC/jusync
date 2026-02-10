#!/usr/bin/env python3
"""
Test client for the simple broker router
Mimics the C++ AnariUsdClient behavior
"""

import zmq
import time
import sys
import struct

def test_string_protocol(endpoint="tcp://localhost:5556"):
    """Test the string protocol for GET_WORKERS"""
    print("🧪 Testing string protocol (GET_WORKERS)...")
    
    context = zmq.Context()
    socket = context.socket(zmq.DEALER)
    socket.setsockopt(zmq.LINGER, 1000)
    socket.setsockopt(zmq.RCVTIMEO, 5000)
    
    # Set identity for debugging
    socket.setsockopt_string(zmq.IDENTITY, "test_client")
    
    print(f"🔌 Connecting to {endpoint}...")
    socket.connect(endpoint)
    
    try:
        # Send GET_WORKERS request (empty frame + string)
        print("📤 Sending GET_WORKERS request...")
        socket.send(b'', zmq.SNDMORE)  # Empty delimiter frame
        socket.send_string("GET_WORKERS")
        
        # Receive response
        print("⏳ Waiting for response...")
        response = socket.recv_string()
        
        print(f"✅ Received response: {response}")
        
        # Parse worker list
        if response.startswith("WORKER_LIST|"):
            worker_data = response[12:]  # Skip "WORKER_LIST|"
            workers = []
            for entry in worker_data.split(';'):
                if entry:
                    parts = entry.split(':')
                    if len(parts) == 3:
                        rank, hostname, ip = parts
                        workers.append((int(rank), hostname, ip))
            
            print(f"📊 Parsed {len(workers)} workers:")
            for rank, hostname, ip in workers:
                print(f"  👷 Rank {rank}: {hostname} ({ip})")
            
            # Total workers = workers in list + broker (rank 0)
            total_workers = len(workers) + 1
            print(f"📈 Total workers (including rank 0): {total_workers}")
            
            return True, total_workers
        else:
            print(f"❌ Invalid response format")
            return False, 0
            
    except zmq.Again:
        print("❌ Timeout waiting for response")
        return False, 0
    except Exception as e:
        print(f"❌ Error: {e}")
        return False, 0
    finally:
        socket.close()
        context.term()

def test_binary_protocol(endpoint="tcp://localhost:5556"):
    """Test binary protocol messages"""
    print("\n🧪 Testing binary protocol...")
    
    context = zmq.Context()
    socket = context.socket(zmq.DEALER)
    socket.setsockopt(zmq.LINGER, 1000)
    socket.setsockopt(zmq.RCVTIMEO, 5000)
    
    socket.setsockopt_string(zmq.IDENTITY, "test_client_binary")
    socket.connect(endpoint)
    
    try:
        # Test 1: REQ_LIST_FILES (message type 100)
        print("📁 Testing REQ_LIST_FILES (type 100)...")
        
        # Create ZmqFileRequest structure
        magic = 0x55534446  # "USDF"
        message_type = 100  # REQ_LIST_FILES
        request_id = 12345
        target_rank = -1  # All ranks
        chunk_size = 0
        
        # Build binary message
        data = bytearray()
        data.extend(struct.pack('<I', magic))  # magic
        data.extend(struct.pack('<I', message_type))  # message_type
        data.extend(struct.pack('<I', request_id))  # request_id
        data.extend(struct.pack('<i', target_rank))  # target_rank (signed)
        data.extend(b'\x00' * 256)  # filename (empty for list request)
        data.extend(struct.pack('<I', chunk_size))  # chunk_size
        
        print(f"📤 Sending binary request ({len(data)} bytes)...")
        socket.send(data)
        
        # Receive response
        print("⏳ Waiting for binary response...")
        response = socket.recv()
        
        print(f"✅ Received binary response ({len(response)} bytes)")
        
        if len(response) >= 20:
            resp_magic = struct.unpack('<I', response[0:4])[0]
            resp_type = struct.unpack('<I', response[4:8])[0]
            print(f"  Response magic: 0x{resp_magic:08X}")
            print(f"  Response type: {resp_type}")
            
            if resp_type == 200:  # RESP_FILE_LIST
                file_count = struct.unpack('<I', response[16:20])[0]
                print(f"  File count: {file_count}")
                
                # Parse filenames if present
                if len(response) > 20:
                    filename_data = response[20:]
                    for i in range(file_count):
                        start = i * 256
                        end = start + 256
                        if end <= len(filename_data):
                            filename = filename_data[start:end].split(b'\x00')[0].decode('utf-8', errors='ignore')
                            print(f"  File {i+1}: {filename}")
        
        # Test 2: REQ_WORKER_STATUS (message type 20)
        print("\n👷 Testing REQ_WORKER_STATUS (type 20)...")
        
        message_type = 20
        request_id = 12346
        
        data = bytearray()
        data.extend(struct.pack('<I', magic))
        data.extend(struct.pack('<I', message_type))
        data.extend(struct.pack('<I', request_id))
        data.extend(struct.pack('<i', 1))  # target_rank = 1
        
        print(f"📤 Sending worker status request ({len(data)} bytes)...")
        socket.send(data)
        
        response = socket.recv()
        print(f"✅ Received response ({len(response)} bytes)")
        
        if len(response) >= 8:
            resp_magic = struct.unpack('<I', response[0:4])[0]
            resp_type = struct.unpack('<I', response[4:8])[0]
            print(f"  Response magic: 0x{resp_magic:08X}")
            print(f"  Response type: {resp_type}")
            
            if resp_type == 22:  # RESP_WORKER_STATUS
                if len(response) >= 216:  # Full ZmqWorkerStatusResponse size
                    source_rank = struct.unpack('<i', response[12:16])[0]
                    worker_status = struct.unpack('<I', response[20:24])[0]
                    print(f"  Source rank: {source_rank}")
                    print(f"  Worker status: {worker_status}")
                    
                    # Parse hostname (bytes 24-88)
                    hostname = response[24:88].split(b'\x00')[0].decode('utf-8', errors='ignore')
                    print(f"  Hostname: {hostname}")
        
        return True
        
    except zmq.Again:
        print("❌ Timeout waiting for response")
        return False
    except Exception as e:
        print(f"❌ Error: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        socket.close()
        context.term()

def test_simple_messages(endpoint="tcp://localhost:5556"):
    """Test simple text messages"""
    print("\n💬 Testing simple text messages...")
    
    context = zmq.Context()
    socket = context.socket(zmq.DEALER)
    socket.setsockopt(zmq.LINGER, 1000)
    socket.setsockopt(zmq.RCVTIMEO, 5000)
    
    socket.setsockopt_string(zmq.IDENTITY, "test_client_text")
    socket.connect(endpoint)
    
    try:
        messages = [
            "Hello from test client!",
            "STATUS",
            "PING",
            "What's the weather like on HPC?"
        ]
        
        for msg in messages:
            print(f"📤 Sending: {msg}")
            socket.send_string(msg)
            
            try:
                reply = socket.recv_string()
                print(f"✅ Reply: {reply}")
            except zmq.Again:
                print("⚠️ No reply received")
            
            time.sleep(0.5)
        
        return True
        
    except Exception as e:
        print(f"❌ Error: {e}")
        return False
    finally:
        socket.close()
        context.term()

def main():
    import argparse
    
    parser = argparse.ArgumentParser(description="Test client for simple broker router")
    parser.add_argument("--endpoint", default="tcp://localhost:5556", help="Broker endpoint")
    parser.add_argument("--test", choices=["all", "string", "binary", "text"], default="all", help="Test to run")
    
    args = parser.parse_args()
    
    print(f"🚀 Starting tests against {args.endpoint}")
    print("=" * 50)
    
    results = {}
    
    if args.test in ["all", "string"]:
        success, worker_count = test_string_protocol(args.endpoint)
        results["string_protocol"] = (success, worker_count)
    
    if args.test in ["all", "binary"]:
        success = test_binary_protocol(args.endpoint)
        results["binary_protocol"] = success
    
    if args.test in ["all", "text"]:
        success = test_simple_messages(args.endpoint)
        results["text_messages"] = success
    
    print("\n" + "=" * 50)
    print("📊 TEST RESULTS:")
    print("=" * 50)
    
    all_passed = True
    for test_name, result in results.items():
        if isinstance(result, tuple):
            success, worker_count = result
            status = "✅ PASS" if success else "❌ FAIL"
            if success and "string" in test_name:
                print(f"{test_name}: {status} (Worker count: {worker_count})")
            else:
                print(f"{test_name}: {status}")
        else:
            status = "✅ PASS" if result else "❌ FAIL"
            print(f"{test_name}: {status}")
        
        if not (isinstance(result, tuple) and result[0] or (isinstance(result, bool) and result)):
            all_passed = False
    
    print("=" * 50)
    if all_passed:
        print("🎉 All tests passed!")
        return 0
    else:
        print("⚠️ Some tests failed")
        return 1

if __name__ == "__main__":
    sys.exit(main())