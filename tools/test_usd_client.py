#!/usr/bin/env python3
"""
Test client for enhanced broker with USD cube generation
Requests and saves colored cubes from different workers
"""

import zmq
import time
import sys
import struct
import os
from pathlib import Path

def test_usd_file_requests(endpoint="tcp://localhost:5556"):
    """Test USD file requests from enhanced broker"""
    print("🧪 Testing USD cube generation from enhanced broker...")
    
    context = zmq.Context()
    socket = context.socket(zmq.DEALER)
    socket.setsockopt(zmq.LINGER, 1000)
    socket.setsockopt(zmq.RCVTIMEO, 10000)
    
    socket.setsockopt_string(zmq.IDENTITY, "usd_test_client")
    socket.connect(endpoint)
    
    # Create output directory
    output_dir = Path("test_usd_output")
    output_dir.mkdir(exist_ok=True)
    
    try:
        # 1. Get worker list
        print("\n1. 📋 Getting worker list...")
        socket.send(b'', zmq.SNDMORE)
        socket.send_string("GET_WORKERS")
        
        response = socket.recv_string()
        print(f"   ✅ Worker list: {response[:50]}...")
        
        # 2. Request file list
        print("\n2. 📁 Requesting file list...")
        
        magic = 0x55534446
        message_type = 100  # REQ_LIST_FILES
        request_id = 1001
        target_rank = -1
        chunk_size = 0
        
        data = bytearray()
        data.extend(struct.pack('<I', magic))
        data.extend(struct.pack('<I', message_type))
        data.extend(struct.pack('<I', request_id))
        data.extend(struct.pack('<i', target_rank))
        data.extend(b'\x00' * 256)
        data.extend(struct.pack('<I', chunk_size))
        
        socket.send(data)
        response = socket.recv()
        
        if len(response) >= 20:
            file_count = struct.unpack('<I', response[16:20])[0]
            print(f"   ✅ File count: {file_count}")
            
            # Parse filenames
            for i in range(file_count):
                start = 20 + i * 256
                end = start + 256
                if end <= len(response):
                    filename = response[start:end].split(b'\x00')[0].decode('utf-8', errors='ignore')
                    print(f"   📄 File {i+1}: {filename}")
        
        # 3. Request individual cube files
        print("\n3. 🎨 Requesting colored cubes from workers...")
        
        # Request cubes for ranks 1-3
        for rank in [1, 2, 3]:
            filename = f"cube_rank{rank}.usda"
            print(f"   📤 Requesting {filename}...")
            
            message_type = 101  # REQ_GET_FILE
            request_id = 1000 + rank
            
            data = bytearray()
            data.extend(struct.pack('<I', magic))
            data.extend(struct.pack('<I', message_type))
            data.extend(struct.pack('<I', request_id))
            data.extend(struct.pack('<i', rank))  # target_rank
            data.extend(filename.encode('utf-8'))
            data.extend(b'\x00' * (256 - len(filename)))
            data.extend(struct.pack('<I', 0))  # chunk_size
            
            socket.send(data)
            
            # Receive file response
            response = socket.recv()
            
            if len(response) >= 288:  # Minimum header size
                # Parse file chunk header
                resp_magic = struct.unpack('<I', response[0:4])[0]
                resp_type = struct.unpack('<I', response[4:8])[0]
                
                if resp_type == 201:  # RESP_FILE_CHUNK
                    # Parse filename from response
                    resp_filename = response[16:272].split(b'\x00')[0].decode('utf-8', errors='ignore')
                    file_size = struct.unpack('<Q', response[272:280])[0]
                    chunk_size = struct.unpack('<I', response[288:292])[0]
                    
                    # Extract file data
                    file_data = response[292:292+chunk_size]
                    
                    # Save to file
                    output_path = output_dir / resp_filename
                    with open(output_path, 'wb') as f:
                        f.write(file_data)
                    
                    print(f"   💾 Saved {resp_filename} ({len(file_data)} bytes)")
                    
                    # Display cube info
                    content = file_data.decode('utf-8', errors='ignore')
                    if 'worker_color' in content:
                        import re
                        color_match = re.search(r'worker_color = "([^"]+)"', content)
                        pos_match = re.search(r'worker_position = \(([^)]+)\)', content)
                        if color_match and pos_match:
                            print(f"   🎨 Color: {color_match.group(1)}, Position: {pos_match.group(1)}")
        
        # 4. Request frame file
        print("\n4. 🎬 Requesting frame file...")
        
        message_type = 102  # REQ_GET_FRAME
        request_id = 2000
        
        data = bytearray()
        data.extend(struct.pack('<I', magic))
        data.extend(struct.pack('<I', message_type))
        data.extend(struct.pack('<I', request_id))
        data.extend(struct.pack('<i', -1))  # all ranks
        data.extend(b'frame_1')
        data.extend(b'\x00' * (256 - 7))
        data.extend(struct.pack('<I', 0))
        
        socket.send(data)
        response = socket.recv()
        
        if len(response) >= 292:
            chunk_size = struct.unpack('<I', response[288:292])[0]
            file_data = response[292:292+chunk_size]
            
            output_path = output_dir / "frame_1.usda"
            with open(output_path, 'wb') as f:
                f.write(file_data)
            
            print(f"   💾 Saved frame_1.usda ({len(file_data)} bytes)")
            
            # Count cubes in frame
            content = file_data.decode('utf-8', errors='ignore')
            cube_count = content.count('def Xform "Cube_Rank')
            print(f"   📊 Frame contains {cube_count} cubes")
        
        print(f"\n✅ All USD files saved to: {output_dir.absolute()}")
        print("   You can open these .usda files in USD-compatible viewers like:")
        print("   - NVIDIA Omniverse")
        print("   - Pixar's usdview")
        print("   - Blender with USD importer")
        
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

def main():
    import argparse
    
    parser = argparse.ArgumentParser(description="Test USD cube generation from enhanced broker")
    parser.add_argument("--endpoint", default="tcp://localhost:5556", help="Broker endpoint")
    
    args = parser.parse_args()
    
    print(f"🚀 Testing enhanced broker at {args.endpoint}")
    print("=" * 60)
    
    success = test_usd_file_requests(args.endpoint)
    
    print("=" * 60)
    if success:
        print("🎉 USD cube generation test successful!")
        return 0
    else:
        print("⚠️ Test failed")
        return 1

if __name__ == "__main__":
    sys.exit(main())