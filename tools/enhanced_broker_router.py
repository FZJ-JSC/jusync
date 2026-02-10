#!/usr/bin/env python3
"""
Enhanced ZMQ ROUTER broker with USD file generation
Generates cubes with different colors/positions for each simulated worker
"""

import zmq
import time
import sys
import json
import random
import struct
from datetime import datetime
from pathlib import Path

class EnhancedBrokerRouter:
    def __init__(self, endpoint="tcp://*:5556"):
        self.endpoint = endpoint
        self.context = zmq.Context()
        self.socket = None
        self.running = False
        self.worker_count = 5
        self.workers = []
        self.save_files = True  # Save generated USD files to disk
        self.cube_colors = [
            (1.0, 0.0, 0.0, 1.0),   # Red
            (0.0, 1.0, 0.0, 1.0),   # Green
            (0.0, 0.0, 1.0, 1.0),   # Blue
            (1.0, 1.0, 0.0, 1.0),   # Yellow
            (1.0, 0.0, 1.0, 1.0),   # Magenta
            (0.0, 1.0, 1.0, 1.0),   # Cyan
            (1.0, 0.5, 0.0, 1.0),   # Orange
            (0.5, 0.0, 1.0, 1.0),   # Purple
        ]
        
    def start(self, worker_count=5):
        """Start the enhanced broker router"""
        self.worker_count = worker_count
        self.workers = []
        
        for i in range(1, worker_count + 1):
            self.workers.append((i, f"worker{i}.hpc.local", f"192.168.1.{100+i}"))
        
        try:
            self.socket = self.context.socket(zmq.ROUTER)
            self.socket.setsockopt(zmq.LINGER, 0)
            self.socket.bind(self.endpoint)
            self.running = True
            
            print(f"🚀 Enhanced Broker Router started on {self.endpoint}")
            print(f"📊 Simulating {self.worker_count} workers")
            print(f"🎨 Cube colors assigned to workers:")
            for i, (rank, hostname, ip) in enumerate(self.workers):
                color = self.cube_colors[i % len(self.cube_colors)]
                color_name = self.get_color_name(color)
                print(f"  👷 Rank {rank}: {color_name} cube at position ({i*2}, 0, 0)")
            
            print("\n📝 Supported commands:")
            print("  - GET_WORKERS (string protocol)")
            print("  - REQ_LIST_FILES (returns cube.usda files)")
            print("  - REQ_GET_FILE cube_rankX.usda (returns colored cube)")
            print("  - REQ_GET_FRAME (returns all cubes for a frame)")
            print("  - REQ_WORKER_STATUS")
            print("\nPress Ctrl+C to stop\n")
            
            return True
        except Exception as e:
            print(f"❌ Failed to start broker: {e}")
            return False
    
    def get_color_name(self, color):
        """Get color name from RGB values"""
        r, g, b, a = color
        if r == 1.0 and g == 0.0 and b == 0.0: return "Red"
        if r == 0.0 and g == 1.0 and b == 0.0: return "Green"
        if r == 0.0 and g == 0.0 and b == 1.0: return "Blue"
        if r == 1.0 and g == 1.0 and b == 0.0: return "Yellow"
        if r == 1.0 and g == 0.0 and b == 1.0: return "Magenta"
        if r == 0.0 and g == 1.0 and b == 1.0: return "Cyan"
        if r == 1.0 and g == 0.5 and b == 0.0: return "Orange"
        if r == 0.5 and g == 0.0 and b == 1.0: return "Purple"
        return f"RGB({r:.1f},{g:.1f},{b:.1f})"

    def save_usd_file(self, filename, data):
        """Save USD data to a file in the current directory"""
        if not self.save_files:
            return
        try:
            import os
            # Ensure directory exists (optional)
            os.makedirs("./usd_output", exist_ok=True)
            filepath = os.path.join("./usd_output", filename)
            with open(filepath, "wb") as f:
                f.write(data)
            print(f"💾 Saved USD file: {filepath} ({len(data)} bytes)")
        except Exception as e:
            print(f"⚠️ Failed to save USD file {filename}: {e}")
    
    def generate_cube_usda(self, rank, color, position=(0, 0, 0), size=1.0):
        """Generate a USD file for a colored cube"""
        r, g, b, a = color
        x, y, z = position
        
        usda_content = f"""#usda 1.0
(
    defaultPrim = "Cube"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "Cube"
{{
    def Mesh "CubeMesh"
    {{
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 3, 2, 4, 5, 7, 6, 0, 4, 5, 1, 1, 5, 7, 3, 3, 7, 6, 2, 2, 6, 4, 0]
        point3f[] points = [({x-size}, {y-size}, {z-size}), ({x+size}, {y-size}, {z-size}), ({x-size}, {y+size}, {z-size}), ({x+size}, {y+size}, {z-size}), ({x-size}, {y-size}, {z+size}), ({x+size}, {y-size}, {z+size}), ({x-size}, {y+size}, {z+size}), ({x+size}, {y+size}, {z+size})]
        color3f[] primvars:displayColor = [({r}, {g}, {b})]
        uniform token subdivisionScheme = "none"
        
        matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }}
    
    custom string worker_rank = "{rank}"
    custom string worker_color = "{self.get_color_name(color)}"
    custom vector3f worker_position = ({x}, {y}, {z})
    
    def Material "CubeMaterial"
    {{
        token outputs:surface.connect = </Cube/CubeMaterial/PBRShader.outputs:surface>
        
        def Shader "PBRShader"
        {{
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = ({r}, {g}, {b})
            float inputs:metallic = 0.0
            float inputs:roughness = 0.5
            token outputs:surface
        }}
    }}
    
    rel material:binding = </Cube/CubeMaterial>
}}
"""
        return usda_content.encode('utf-8')
    
    def generate_frame_usda(self, frame_number):
        """Generate a USD file with all cubes for a frame"""
        usda_content = """#usda 1.0
(
    defaultPrim = "Frame"
    metersPerUnit = 1
    upAxis = "Y"
    framesPerSecond = 24
    timeCodesPerSecond = 24
    startTimeCode = 0
    endTimeCode = 100
)

def Xform "Frame"
{
    custom int frame_number = %d
    
""" % frame_number
        
        # Add cubes from all workers
        for i, (rank, hostname, ip) in enumerate(self.workers):
            color = self.cube_colors[i % len(self.cube_colors)]
            position = (i * 3.0, 0, 0)  # Spread cubes along X axis
            r, g, b, a = color
            
            usda_content += f"""
    def Xform "Cube_Rank{rank}"
    {{
        def Mesh "CubeMesh"
        {{
            int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
            int[] faceVertexIndices = [0, 1, 3, 2, 4, 5, 7, 6, 0, 4, 5, 1, 1, 5, 7, 3, 3, 7, 6, 2, 2, 6, 4, 0]
            point3f[] points = [({position[0]-1}, {position[1]-1}, {position[2]-1}), ({position[0]+1}, {position[1]-1}, {position[2]-1}), ({position[0]-1}, {position[1]+1}, {position[2]-1}), ({position[0]+1}, {position[1]+1}, {position[2]-1}), ({position[0]-1}, {position[1]-1}, {position[2]+1}), ({position[0]+1}, {position[1]-1}, {position[2]+1}), ({position[0]-1}, {position[1]+1}, {position[2]+1}), ({position[0]+1}, {position[1]+1}, {position[2]+1})]
            color3f[] primvars:displayColor = [({r}, {g}, {b})]
            uniform token subdivisionScheme = "none"
            
            matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), ({position[0]}, {position[1]}, {position[2]}, 1) )
            uniform token[] xformOpOrder = ["xformOp:transform"]
        }}
        
        custom int worker_rank = {rank}
        custom string worker_color = "{self.get_color_name(color)}"
        custom vector3f worker_position = ({position[0]}, {position[1]}, {position[2]})
        
        def Material "CubeMaterial"
        {{
            token outputs:surface.connect = </Frame/Cube_Rank{rank}/CubeMaterial/PBRShader.outputs:surface>
            
            def Shader "PBRShader"
            {{
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = ({r}, {g}, {b})
                float inputs:metallic = 0.0
                float inputs:roughness = 0.5
                token outputs:surface
            }}
        }}
        
        rel material:binding = </Frame/Cube_Rank{rank}/CubeMaterial>
    }}
"""
        
        usda_content += "}\n"
        return usda_content.encode('utf-8')
    
    def stop(self):
        """Stop the broker router"""
        self.running = False
        if self.socket:
            self.socket.close()
        self.context.term()
        print("\n🛑 Enhanced broker stopped")
    
    def handle_get_workers(self, identity):
        """Handle GET_WORKERS string request"""
        worker_list = []
        for rank, hostname, ip in self.workers:
            worker_list.append(f"{rank}:{hostname}:{ip}")
        
        response = f"WORKER_LIST|{';'.join(worker_list)}"
        
        self.socket.send(identity, zmq.SNDMORE)
        self.socket.send_string(response)
        
        print(f"📤 Sent worker list to {identity.hex()[:8]}: {len(self.workers)} workers")
        return True
    
    def handle_binary_request(self, identity, data):
        """Handle binary protocol requests"""
        try:
            if len(data) < 8:
                print(f"⚠️ Invalid binary message size: {len(data)} bytes")
                return False
            
            magic = int.from_bytes(data[0:4], byteorder='little')
            message_type = int.from_bytes(data[4:8], byteorder='little')
            
            print(f"📦 Binary message from {identity.hex()[:8]}:")
            print(f"  Magic: 0x{magic:08X}")
            print(f"  Type: {message_type}")
            
            if magic == 0x55534446:  # "USDF"
                if message_type == 100:  # REQ_LIST_FILES
                    print("  📁 REQ_LIST_FILES request")
                    response = self.create_file_list_response()  # Returns (header, data) tuple
                    self.send_binary_response(identity, response)
                    return True
                elif message_type == 101:  # REQ_GET_FILE
                    print("  📄 REQ_GET_FILE request")
                    if len(data) >= 272:
                        filename = data[16:272].split(b'\x00')[0].decode('utf-8', errors='ignore')
                        print(f"  Requested file: {filename}")
                        
                        # Check if it's a cube file
                        if filename.startswith("cube_rank") and filename.endswith(".usda"):
                            try:
                                rank_str = filename.replace("cube_rank", "").replace(".usda", "")
                                rank = int(rank_str)
                                if 1 <= rank <= self.worker_count:
                                    # Generate cube for this rank
                                    color_idx = (rank - 1) % len(self.cube_colors)
                                    position = ((rank - 1) * 3.0, 0, 0)
                                    cube_data = self.generate_cube_usda(rank, self.cube_colors[color_idx], position)
                                    response = self.create_file_response(filename, cube_data)
                                    self.send_binary_response(identity, response)
                                    print(f"  🎨 Generated {self.get_color_name(self.cube_colors[color_idx])} cube for rank {rank}")
                                    return True
                            except ValueError:
                                pass
                        
                        # Default: send generic cube
                        cube_data = self.generate_cube_usda(1, self.cube_colors[0])
                        response = self.create_file_response("default_cube.usda", cube_data)
                        self.send_binary_response(identity, response)
                    return True
                elif message_type == 102:  # REQ_GET_FRAME
                    print("  🎬 REQ_GET_FRAME request")
                    # Parse frame number from filename
                    if len(data) >= 272:
                        filename = data[16:272].split(b'\x00')[0].decode('utf-8', errors='ignore')
                        if filename.startswith("frame_"):
                            try:
                                frame_num = int(filename.replace("frame_", ""))
                                frame_data = self.generate_frame_usda(frame_num)
                                response = self.create_file_response(f"frame_{frame_num}.usda", frame_data)
                                self.send_binary_response(identity, response)
                                print(f"  🎬 Generated frame {frame_num} with {self.worker_count} cubes")
                                return True
                            except ValueError:
                                pass
                    
                    # Default: frame 1
                    frame_data = self.generate_frame_usda(1)
                    response = self.create_file_response("frame_1.usda", frame_data)
                    self.send_binary_response(identity, response)
                    return True
                elif message_type == 20:  # REQ_WORKER_STATUS
                    print("  👷 REQ_WORKER_STATUS request")
                    response = self.create_worker_status_response()
                    self.send_binary_response(identity, response)
                    return True
                else:
                    print(f"  ⚠️ Unsupported binary message type: {message_type}")
                    return False
            else:
                print(f"  ❌ Invalid magic number: 0x{magic:08X}")
                return False
                
        except Exception as e:
            print(f"❌ Error handling binary request: {e}")
            import traceback
            traceback.print_exc()
            return False
    
    def create_file_list_response(self):
        """Create HPC-compliant file list response with JSON data"""
        # Create JSON data with file list
        files = []
        for rank, _, _ in self.workers:
            filename = f"cube_rank{rank}.usda"
            files.append({
                "name": filename,
                "size": 1024,  # Approximate size
                "mime": "application/usd"
            })
        
        # Add frame file
        files.append({
            "name": "frame_1.usda",
            "size": 2048,
            "mime": "application/usd"
        })
        
        json_data = {
            "rank": 0,
            "files": files
        }
        
        json_str = json.dumps(json_data)
        json_bytes = json_str.encode('utf-8')
        
        # Create ZmqFileChunk header (292 bytes)
        magic = 0x55534446
        message_type = 201  # RESP_FILE_CHUNK
        request_id = 1
        source_rank = 0
        filename = "filelist.json"
        file_size = len(json_bytes)
        chunk_offset = 0
        chunk_size = len(json_bytes)
        
        header = bytearray()
        header.extend(magic.to_bytes(4, 'little'))
        header.extend(message_type.to_bytes(4, 'little'))
        header.extend(request_id.to_bytes(4, 'little'))
        header.extend(source_rank.to_bytes(4, 'little', signed=True))
        
        # Filename (256 bytes)
        filename_bytes = filename.encode('utf-8')[:255]
        header.extend(filename_bytes)
        header.extend(b'\x00' * (256 - len(filename_bytes)))
        
        header.extend(file_size.to_bytes(8, 'little'))
        header.extend(chunk_offset.to_bytes(8, 'little'))
        header.extend(chunk_size.to_bytes(4, 'little'))
        
        # Create ZmqFileComplete message (280 bytes)
        complete = bytearray()
        complete.extend(magic.to_bytes(4, 'little'))
        complete.extend((202).to_bytes(4, 'little'))  # RESP_FILE_COMPLETE = 202
        complete.extend(request_id.to_bytes(4, 'little'))
        complete.extend(source_rank.to_bytes(4, 'little', signed=True))
        
        # Filename (256 bytes)
        complete.extend(filename_bytes)
        complete.extend(b'\x00' * (256 - len(filename_bytes)))
        
        complete.extend(file_size.to_bytes(8, 'little'))
        
        # HPC broker sends: ZmqFileChunk header + JSON data combined in single message
        # Create combined header + JSON buffer
        combined = bytearray(header)
        combined.extend(json_bytes)
        
        # Return two-part response: combined header+JSON + complete
        return bytes(combined), bytes(complete)
    
    def create_file_response(self, filename, file_data):
        """Create a file response with the actual file data - returns two-part HPC response (combined header+data, complete)"""
        # Save USD file to disk for inspection
        self.save_usd_file(filename, file_data)
        
        magic = 0x55534446
        request_id = 1
        source_rank = 1
        file_size = len(file_data)
        
        # Create ZmqFileChunk header (292 bytes)
        header = bytearray()
        header.extend(magic.to_bytes(4, 'little'))
        header.extend((201).to_bytes(4, 'little'))  # RESP_FILE_CHUNK = 201
        header.extend(request_id.to_bytes(4, 'little'))
        header.extend(source_rank.to_bytes(4, 'little', signed=True))
        
        # Filename (256 bytes)
        filename_bytes = filename.encode('utf-8')[:255]
        header.extend(filename_bytes)
        header.extend(b'\x00' * (256 - len(filename_bytes)))
        
        header.extend(file_size.to_bytes(8, 'little'))
        header.extend((0).to_bytes(8, 'little'))  # chunk_offset = 0
        header.extend(file_size.to_bytes(4, 'little'))  # chunk_size = file_size
        
        # Create ZmqFileComplete message (280 bytes)
        complete = bytearray()
        complete.extend(magic.to_bytes(4, 'little'))
        complete.extend((202).to_bytes(4, 'little'))  # RESP_FILE_COMPLETE = 202
        complete.extend(request_id.to_bytes(4, 'little'))
        complete.extend(source_rank.to_bytes(4, 'little', signed=True))
        
        # Filename (256 bytes)
        complete.extend(filename_bytes)
        complete.extend(b'\x00' * (256 - len(filename_bytes)))
        
        complete.extend(file_size.to_bytes(8, 'little'))
        
        # Return two-part HPC response: combined header+data, then complete
        combined = bytes(header) + file_data
        return combined, bytes(complete)
    
    def create_worker_status_response(self):
        """Create worker status response"""
        magic = 0x55534446
        message_type = 22
        request_id = 1
        source_rank = 1
        worker_count = self.worker_count
        worker_status = 1  # Idle
        last_heartbeat = int(time.time())
        
        response = bytearray()
        response.extend(magic.to_bytes(4, 'little'))
        response.extend(message_type.to_bytes(4, 'little'))
        response.extend(request_id.to_bytes(4, 'little'))
        response.extend(source_rank.to_bytes(4, 'little', signed=True))
        response.extend(worker_count.to_bytes(4, 'little'))
        response.extend(worker_status.to_bytes(4, 'little'))
        response.extend(last_heartbeat.to_bytes(8, 'little'))
        
        hostname = "worker1.hpc.local".encode('utf-8')
        response.extend(hostname[:63])
        response.extend(b'\x00' * (64 - len(hostname)))
        
        gpu_info = "NVIDIA A100 80GB".encode('utf-8')
        response.extend(gpu_info[:127])
        response.extend(b'\x00' * (128 - len(gpu_info)))
        
        return bytes(response)
    
    def send_binary_response(self, identity, data):
        """Send binary response to client exactly like actual HPC broker.
        data can be either:
        - bytes: single message response (for file data, worker status, etc.)
        - tuple (combined, complete): two-part HPC response (combined header+JSON + complete)
        Each message is sent as three ZeroMQ frames: identity, empty delimiter, msgData.
        """
        def send_single(msg_data):
            # Three frames: identity, empty delimiter, msgData
            self.socket.send(identity, zmq.SNDMORE)
            self.socket.send(zmq.Frame(b''), zmq.SNDMORE)  # empty delimiter
            self.socket.send(msg_data)
            print(f"📤 Sent binary response ({len(msg_data)} bytes) to {identity.hex()[:8]}")
        
        if isinstance(data, tuple) and len(data) == 2:
            # Two-part HPC response: send combined, then complete as separate messages
            combined, complete = data
            send_single(combined)
            send_single(complete)
            print(f"📤 Sent HPC two-part response: combined={len(combined)} bytes, complete={len(complete)} bytes to {identity.hex()[:8]}")
        else:
            # Single message response
            send_single(data)
    
    def handle_text_message(self, identity, message):
        """Handle simple text messages"""
        print(f"💬 Text message from {identity.hex()[:8]}: {message}")
        
        reply = f"ACK: {message}"
        self.socket.send(identity, zmq.SNDMORE)
        self.socket.send_string(reply)
        
        print(f"📤 Sent reply: {reply}")
        return True
    
    def run(self):
        """Main broker loop"""
        poller = zmq.Poller()
        poller.register(self.socket, zmq.POLLIN)
        
        while self.running:
            try:
                socks = dict(poller.poll(1000))
                
                if self.socket in socks:
                    identity = self.socket.recv()
                    
                    try:
                        # Try to receive message with RCVMORE check
                        message = self.socket.recv(zmq.RCVMORE)
                        more = self.socket.getsockopt(zmq.RCVMORE)
                        
                        # Check if this is empty delimiter frame (zero-length) with more data
                        if len(message) == 0 and more:
                            # Skip empty delimiter, read the actual payload
                            payload = self.socket.recv()
                            more = self.socket.getsockopt(zmq.RCVMORE)
                            # There should be no more frames after payload
                            # Determine if payload is string or binary
                            try:
                                # Try to decode as UTF-8 string
                                string_msg = payload.decode('utf-8')
                                if string_msg == "GET_WORKERS":
                                    self.handle_get_workers(identity)
                                else:
                                    self.handle_text_message(identity, string_msg)
                                continue
                            except UnicodeDecodeError:
                                # Not a valid string, treat as binary
                                pass
                            # Check for binary magic
                            if len(payload) >= 4:
                                magic = int.from_bytes(payload[0:4], byteorder='little')
                                if magic == 0x55534446:  # "USDF" magic
                                    # It's a binary message, check for more parts
                                    data = payload
                                    if more:
                                        while more:
                                            next_part = self.socket.recv()
                                            data += next_part
                                            more = self.socket.getsockopt(zmq.RCVMORE)
                                    self.handle_binary_request(identity, data)
                                    continue
                            # Fallback: treat as text with errors ignored
                            text_msg = payload.decode('utf-8', errors='ignore')
                            self.handle_text_message(identity, text_msg)
                            continue
                        
                        # If message is not empty delimiter, treat as direct message (legacy)
                        # Check if this is binary data
                        if len(message) >= 4:
                            # Try to parse as binary message
                            try:
                                magic = int.from_bytes(message[0:4], byteorder='little')
                                if magic == 0x55534446:  # "USDF" magic
                                    # It's a binary message, check for more parts
                                    data = message
                                    if more:
                                        while more:
                                            next_part = self.socket.recv()
                                            data += next_part
                                            more = self.socket.getsockopt(zmq.RCVMORE)
                                    self.handle_binary_request(identity, data)
                                    continue
                            except:
                                pass
                        
                        # Fallback: treat as text message
                        try:
                            text_msg = message.decode('utf-8', errors='ignore')
                            self.handle_text_message(identity, text_msg)
                        except:
                            print(f"⚠️ Could not parse message from {identity.hex()[:8]}")
                            
                    except zmq.ZMQError as e:
                        print(f"⚠️ ZMQ error: {e}")
                        continue
                        
            except KeyboardInterrupt:
                print("\n🛑 Received interrupt, shutting down...")
                self.running = False
                break
            except Exception as e:
                print(f"❌ Error in broker loop: {e}")
                import traceback
                traceback.print_exc()
                time.sleep(1)

def main():
    import argparse
    
    parser = argparse.ArgumentParser(description="Enhanced ZMQ ROUTER broker with USD cube generation")
    parser.add_argument("--endpoint", default="tcp://*:5556", help="ZMQ endpoint to bind to")
    parser.add_argument("--workers", type=int, default=5, help="Number of simulated workers")
    
    args = parser.parse_args()
    
    broker = EnhancedBrokerRouter(args.endpoint)
    
    if not broker.start(args.workers):
        return 1
    
    try:
        broker.run()
    finally:
        broker.stop()
    
    return 0

if __name__ == "__main__":
    sys.exit(main())