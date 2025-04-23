#!/usr/bin/env python3

import socket
import time
import numpy as np
import subprocess
import os
import signal
import atexit
import sys
import pygame 



class AllegroHand:
    def __init__(self, host='localhost', port=12321, grasp_path=None):
        """Initialize connection to Allegro Hand server
        
        Args:
            host: Server hostname
            port: Server port
            grasp_path: Path to the grasp executable. If None, will try to find it
        """
        self.host = host
        self.port = port
        self.socket = None
        self.grasp_process = None
        
        # Initialize pygame and joystick
        pygame.init()
        pygame.joystick.init()
        
        # Try to find a joystick
        if pygame.joystick.get_count() > 0:
            self.joystick = pygame.joystick.Joystick(0)
            self.joystick.init()
            print(f"Initialized joystick: {self.joystick.get_name()}")
        else:
            print("No joystick found!")
            self.joystick = None
        
        # Find grasp executable
        if grasp_path is None:
            # Try common locations
            possible_paths = [
                './build/grasp/grasp',
                os.path.join(os.path.dirname(__file__), 'grasp')
            ]
            for path in possible_paths:
                if os.path.exists(path):
                    grasp_path = path
                    break
            if grasp_path is None:
                raise FileNotFoundError("Could not find grasp executable. Please specify path.")
        
        self.grasp_path = os.path.abspath(grasp_path)
        
        # Register cleanup on exit
        atexit.register(self.cleanup)
        
        # Start grasp program
        self.start_grasp()
        
        # Wait a bit for the server to start
        time.sleep(1)
        
        # Connect to the server
        self.connect()
        
    def start_grasp(self):
        """Start the grasp program"""
        try:
            print(f"Starting {self.grasp_path}...")
            # Start process and redirect output to /dev/null
            with open(os.devnull, 'w') as devnull:
                self.grasp_process = subprocess.Popen(
                    self.grasp_path,
                    stdout=devnull,
                    stderr=devnull,
                    preexec_fn=os.setsid  # Create new process group
                )
        except Exception as e:
            print(f"Failed to start grasp program: {e}")
            sys.exit(1)
        
    def cleanup(self):
        """Cleanup resources"""
        print("Ending connection & Cleaning up...")
        if self.socket:
            try:
                # Try to send a quit command to the grasp program
                self.socket.send("QUIT\n".encode())
                time.sleep(0.1)  # Give it a moment to process
            except:
                pass  # Ignore any socket errors during cleanup
            self.close()
        
        if self.grasp_process:
            try:
                # Check if process is still running
                if self.grasp_process.poll() is None:
                    # Process is still running, try to terminate it gracefully
                    os.killpg(os.getpgid(self.grasp_process.pid), signal.SIGTERM)
                    try:
                        self.grasp_process.wait(timeout=2)  # Wait up to 2 seconds
                    except subprocess.TimeoutExpired:
                        # If still running after SIGTERM, force kill
                        if self.grasp_process.poll() is None:
                            os.killpg(os.getpgid(self.grasp_process.pid), signal.SIGKILL)
            except ProcessLookupError:
                # Process is already gone, which is fine
                pass
            except Exception as e:
                # Log other errors but don't raise
                print(f"Warning during cleanup: {e}", file=sys.stderr)
            finally:
                self.grasp_process = None
        
    def connect(self):
        """Connect to the Allegro Hand server"""
        max_attempts = 5
        attempt = 0
        while attempt < max_attempts:
            try:
                self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.socket.connect((self.host, self.port))
                print(f"Connected to Allegro Hand server at {self.host}:{self.port}")
                return
            except Exception as e:
                attempt += 1
                if attempt < max_attempts:
                    print(f"Connection attempt {attempt} failed: {e}. Retrying...")
                    time.sleep(1)
                else:
                    print(f"Failed to connect after {max_attempts} attempts: {e}")
                    self.cleanup()
                    sys.exit(1)
            
    def set_joint_positions(self, positions):
        """Set joint positions for all joints
        
        Args:
            positions: List/array of 16 joint angles in radians
        """
        if len(positions) != 16:
            raise ValueError("Must provide exactly 16 joint positions")
            
        if not self.socket:
            print("Not connected to server")
            return False
            
        try:
            # Format command string
            cmd = "SET_JOINTS " + " ".join([f"{p:.6f}" for p in positions]) + "\n"
            self.socket.send(cmd.encode())
            
            # Wait for acknowledgment
            response = self.socket.recv(1024).decode().strip()
            return response == "OK"
        except Exception as e:
            print(f"Failed to send joint positions: {e}")
            return False

    def get_joint_positions(self):
        """Get current joint positions for all joints
        
        Returns:
            numpy array of 16 joint angles in radians, or None if error
        """
        if not self.socket:
            print("Not connected to server")
            return None
            
        try:
            # Send command
            self.socket.send("GET_JOINTS\n".encode())
            
            # Read response
            response = self.socket.recv(1024).decode().strip()
            
            # Parse joint positions
            positions = np.array([float(x) for x in response.split()])
            if len(positions) != 16:
                raise ValueError(f"Expected 16 joint positions, got {len(positions)}")
                
            return positions
            
        except Exception as e:
            print(f"Failed to get joint positions: {e}")
            return None

    def get_joint_torques(self):
        """Get current joint torques for all joints
        
        Returns:
            numpy array of 16 joint torques, or None if error
        """
        if not self.socket:
            print("Not connected to server")
            return None
            
        try:
            # Send command
            self.socket.send("GET_TORQUES\n".encode())
            
            # Read response
            response = self.socket.recv(1024).decode().strip()
            
            # Parse joint torques
            torques = np.array([float(x) for x in response.split()])
            if len(torques) != 16:
                raise ValueError(f"Expected 16 joint torques, got {len(torques)}")
                
            return torques
            
        except Exception as e:
            print(f"Failed to get joint torques: {e}")
            return None

    def demo_move_joints_cycle(self):
        """Move joints in a cyclic pattern from 0 to 1.2 radians and back"""
        steps = 10  # Number of steps to take
        
        while True:
            # Move joints from 0 to 1.2 radians
            for step in range(steps + 1):
                # print(self.get_joint_positions())
                # Calculate current angle for this step
                angle = (1.0 * step) / steps
                
                # Set all joints to current angle except 0,4,8
                positions = np.ones(16) * angle
                positions[0] = 0  # Keep joint 0 at 0
                positions[4] = 0  # Keep joint 4 at 0 
                positions[8] = 0  # Keep joint 8 at 0
                
                # print(f"Setting joints to {angle:.3f} radians...")
                self.set_joint_positions(positions)
                time.sleep(0.05)  # Small delay between steps

            # Now spread back to zeros
            for step in range(steps + 1):
                # print(self.get_joint_positions())
                # Calculate current angle for this step, going from 1.2 to 0
                angle = 1.2 * (steps - step) / steps
                
                # Set all joints to current angle except 0,4,8
                positions = np.ones(16) * angle
                positions[0] = 0  # Keep joint 0 at 0
                positions[4] = 0  # Keep joint 4 at 0
                positions[8] = 0  # Keep joint 8 at 0
                
                self.set_joint_positions(positions)
                time.sleep(0.05)  # Small delay between steps

    def demo_read_joint_pos_and_force(self):
        """Read joint positions"""
        self.demo_pose_four()
        while True:
            positions = self.get_joint_positions()
            torques = self.get_joint_torques()
            
            # Print positions for each finger
            print("Joint Positions:")
            print(f"Index:     {positions[0:4]}")
            print(f"Middle:     {positions[4:8]}")
            print(f"Ring:    {positions[8:12]}")
            print(f"Thumb:      {positions[12:16]}")
            
            # Print torques for each finger
            print("Joint Torques:")
            print(f"Index:     {torques[0:4]}")
            print(f"Middle:     {torques[4:8]}")
            print(f"Ring:    {torques[8:12]}")
            print(f"Thumb:      {torques[12:16]}")

            print("--------------------------------")
            time.sleep(0.1)

    def demo_pose_one(self):
        """Move joints to a specific pose"""
        positions = np.ones(16)
        positions[0:4] = 0.0
        positions[4] = 0.0
        positions[8] = 0.0
        self.set_joint_positions(positions)
        time.sleep(1)  

    def demo_pose_two(self):
        """Move joints to a specific pose"""
        positions = np.ones(16)
        positions[0:8] = 0.0
        positions[8] = 0.0
        self.set_joint_positions(positions)
        time.sleep(1)  

    def demo_pose_three(self):
        """Move joints to a specific pose"""
        positions = np.ones(16)
        positions[0:12] = 0.0
        self.set_joint_positions(positions)
        time.sleep(1)  

    def demo_pose_four(self):
        """Move joints to a specific pose"""
        positions = np.zeros(16)
        self.set_joint_positions(positions)
        time.sleep(1)  

    def demo_pose_six(self):
        """Move joints to a specific pose"""
        positions = 1.3*np.ones(16)
        positions[8:16] = 0

        positions[0] = 0.0
        positions[4] = 0.0
        positions[8] = 0.0

        self.set_joint_positions(positions)
        time.sleep(1)  

    def demo_pose_seven(self):
        """Move joints to a specific pose"""
        positions = np.ones(16)

        positions[:8] = 0.7
        positions[1] = 1
        positions[5] = 1
        positions[8:12] = 1.4


        positions[12:] = 0.9
        positions[13] = 0.4
        positions[14] = 0.5

        positions[0] = 0.0
        positions[4] = 0.0
        positions[8] = 0.0

        self.set_joint_positions(positions)
        time.sleep(1)  

    def demo_pose_eight(self):
        """Move joints to a specific pose"""
        positions = np.zeros(16)
        positions[4:12] = 1.4
        positions[0] = 0.0
        positions[4] = 0.0
        positions[8] = 0.0
        self.set_joint_positions(positions)
        time.sleep(1)  

    def demo_pose_nine(self):
        """Move joints to a specific pose"""
        positions = np.zeros(16)
        positions[2:4] = 1.3
        positions[4:12] = 1.3
        positions[12:16] = 1.0
        positions[0] = 0.0
        positions[4] = 0.0
        positions[8] = 0.0
        self.set_joint_positions(positions)
        time.sleep(1)  
    
    def demo_pose_fist(self):
        """Move joints to a specific pose"""
        positions = np.ones(16)
        positions[0] = 0.0
        positions[4] = 0.0
        positions[8] = 0.0
        self.set_joint_positions(positions)
        time.sleep(1)  

    def close(self):
        """Close connection to server"""
        if self.socket:
            self.socket.close()
            self.socket = None

    def joystick_control(self):
        """Control hand spreading/contraction using joystick axis"""
        if not self.joystick:
            print("No joystick available")
            return
            
        print("Starting joystick control. Press Ctrl+C to exit.")
        print("Use the last but two axis to control hand spreading/contraction")
        print("Push up to spread, down to contract")
        
        # Initialize hand position
        current_hand_angle = 0.0
        max_hand_angle = 1.1
        
        try:
            while True:
                pygame.event.pump()  # Process pygame events
                
                spread_control = self.joystick.get_axis(4) # Invert so that up spreads and down contracts

                current_hand_angle = ((spread_control + 1) / 2) * max_hand_angle
            
                positions = np.ones(16) * current_hand_angle
                positions[0] = 0  # Keep joint 0 at 0
                positions[4] = 0  # Keep joint 4 at 0
                positions[8] = 0  # Keep joint 8 at 0
                
                self.set_joint_positions(positions)
                
                print(f"\rHand angle: {current_hand_angle:.3f} rad", end='')
                sys.stdout.flush()
                
                time.sleep(0.02)  # Small delay to prevent overwhelming the system
                
        except KeyboardInterrupt:
            print("\nJoystick control terminated")
        except Exception as e:
            print(f"\nError in joystick control: {e}")


def demo_hand_joystick():
    hand = AllegroHand(grasp_path='./build/grasp/grasp')
    hand.joystick_control()

def demo_hand_gestures():
    hand = AllegroHand(grasp_path='./build/grasp/grasp')
    hand.demo_pose_fist()
    hand.demo_pose_one()
    hand.demo_pose_two()
    hand.demo_pose_three()
    hand.demo_pose_four()
    hand.demo_pose_six()
    hand.demo_pose_seven()
    hand.demo_pose_eight()
    hand.demo_pose_nine()


if __name__ == "__main__":
    # staic gestures
    demo_hand_gestures()

    # while loop interactive control
    # demo_hand_joystick()
