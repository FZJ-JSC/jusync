#!/usr/bin/env python3
"""
Test script to verify the two-part response format for file list requests.
This simulates what the C++ client expects.
"""

import zmq
import struct
import time

def test_file_list_request():
    """Test REQ_LIST_FILES request and verify two-part response"""
    
    # Create DEALER socket (like the C++ client)
    context = zmq.Context()
    socket = context.socket(zmq.DEALER)
    socket.setsockopt(zmq.RCVTIMEO, 5000)  # 5 second timeout
    socket.connect("tcp://localhost:5555")
    
    print("🔌 Connected to broker at tcp://localhost:5555")
    
    # Create REQ_LIST_FILES message (binary format)
    magic = 0x55534446  # "USDF"
    message_type = 100   # REQ_LIST_FILES
    request_id = 12345
    source_rank = -1
    
    request = bytearray()
    request.extend(magic.to_bytes(4, 'little'))
    request.extend(message_type.to_bytes(4, 'little'))
    request.extend(request_id.to_bytes(4, 'little'))
    request.extend(source_rank.to_bytes(4, 'little', signed=True))
    
    print(f"📤 Sending REQ_LIST_FILES request ({len(request)} bytes)")
    socket.send(request)
    
    # Receive response - should be two messages
    try:
        # First message (header)
        header = socket.recv()
        print(f"📥 Received header: {len(header)} bytes")
        
        # Check if there's more data
        more = socket.getsockopt(zmq.RCVMORE)
        print(f"  More parts: {more}")
        
        if more:
            # Second message (file data)
            data = socket.recv()
            print(f"📥 Received data: {len(data)} bytes")
            
            # Parse header
            if len(header) >= 20:
                resp_magic = struct.unpack('<I', header[0:4])[0]
                resp_type = struct.unpack('<I', header[4:8])[0]
                resp_req_id = struct.unpack('<I', header[8:12])[0]
                resp_source_rank = struct.unpack('<i', header[12:16])[0]
                resp_file_count = struct.unpack('<I', header[16:20])[0]
                
                print(f"  Magic: 0x{resp_magic:08X} (USDF)")
                print(f"  Type: {resp_type} (RESP_FILE_LIST)")
                print(f"  Request ID: {resp_req_id}")
                print(f"  Source rank: {resp_source_rank}")
                print(f"  File count: {resp_file_count}")
                
                # Parse filenames from data
                filename_size = 256
                for i in range(resp_file_count):
                    start = i * filename_size
                    end = start + filename_size
                    if end <= len(data):
                        filename_bytes = data[start:end]
                        filename = filename_bytes.split(b'\x00')[0].decode('utf-8', errors='ignore')
                        print(f"  File {i+1}: {filename}")
            
            # Check if there are more messages (should be false)
            more = socket.getsockopt(zmq.RCVMORE)
            print(f"  More parts after data: {more}")
            
            if more:
                print("⚠️  Warning: Unexpected additional message parts")
                # Read any remaining messages
                while more:
                    extra = socket.recv()
                    print(f"  Extra part: {len(extra)} bytes")
                    more = socket.getsockopt(zmq.RCVMORE)
        else:
            print("❌ Error: Expected two-part response but got single message")
            
    except zmq.Again:
        print("❌ Timeout waiting for response")
    
    socket.close()
    context.term()
    print("✅ Test completed")

if __name__ == "__main__":
    print("🚀 Testing two-part response format")
    print("Make sure enhanced_broker_router.py is running first!")
    print("Run: python tools/enhanced_broker_router.py --workers 5")
    print()
    
    try:
        test_file_list_request()
    except Exception as e:
        print(f"❌ Error: {e}")
        import traceback
        traceback.print_exc()