# Troubleshooting Guide

Common issues and solutions for Soluna.

## Quick Diagnostics

```bash
# Check service status
sudo systemctl status soluna

# View recent logs
sudo journalctl -u soluna -n 100

# Check audio devices
solunad --list-devices

# Check network
solunad --status

# Test audio output
speaker-test -c 2 -t wav
```

## Audio Issues

### No Audio Output

**Symptoms:** Service running but no sound

**Solutions:**

1. **Check audio device:**
   ```bash
   # List devices
   aplay -l

   # Test direct playback
   speaker-test -c 2

   # Check config device name
   cat /etc/soluna/config.yaml | grep audio
   ```

2. **Check permissions:**
   ```bash
   # Add user to audio group
   sudo usermod -a -G audio $USER
   # Or for service user
   sudo usermod -a -G audio soluna

   # Restart service
   sudo systemctl restart soluna
   ```

3. **Check ALSA mixer:**
   ```bash
   alsamixer
   # Unmute with M key, adjust volume with arrows
   ```

4. **Verify stream is active:**
   ```bash
   solctl streams
   solctl routes
   ```

### Audio Glitches / Dropouts

**Symptoms:** Clicks, pops, stuttering

**Solutions:**

1. **Increase buffer size:**
   ```yaml
   # In config.yaml
   audio:
     frames_per_packet: 96   # Increase from 48
     buffer_packets: 12      # Increase from 8
   ```

2. **Check CPU governor:**
   ```bash
   # Set to performance mode
   echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
   ```

3. **Enable real-time priority:**
   ```bash
   # Edit /etc/security/limits.conf
   @audio - rtprio 99
   @audio - memlock unlimited
   ```

4. **Check network quality:**
   ```bash
   # Monitor packet loss
   watch -n 1 'solctl status'

   # Use wired connection if on WiFi
   ```

5. **Check for interference:**
   ```bash
   # View metrics
   curl http://localhost:9100/metrics | grep soluna_rtp_packets_lost
   ```

### High Latency

**Symptoms:** Noticeable delay between source and output

**Solutions:**

1. **Reduce buffer:**
   ```yaml
   audio:
     frames_per_packet: 48   # 1ms at 48kHz
     buffer_packets: 4       # Minimum safe value
   ```

2. **Use wired network:**
   - Switch from WiFi to Ethernet
   - Latency: WiFi ~20-50ms, Ethernet ~2-5ms

3. **Check PTP sync:**
   ```bash
   solctl status
   # ptp_offset_ns should be < 1000000 (1ms)
   ```

4. **Disable WiFi power saving:**
   ```bash
   sudo iw wlan0 set power_save off
   ```

### Distorted Audio

**Symptoms:** Clipping, distortion, wrong pitch

**Solutions:**

1. **Check gain levels:**
   ```bash
   solctl routes
   # Reduce gain if > 0 dB
   solctl route gain --source X --sink Y --db -6
   ```

2. **Verify sample rate match:**
   ```yaml
   # All devices must use same rate
   audio:
     sample_rate: 48000
   ```

3. **Check meters for clipping:**
   ```bash
   solctl meters
   # peak_db should be < -3 dB
   ```

## Network Issues

### Devices Not Discovered

**Symptoms:** `solctl devices` shows empty or missing devices

**Solutions:**

1. **Check network connectivity:**
   ```bash
   # Verify IP addresses
   ip addr show

   # Ping other device
   ping 192.168.1.x
   ```

2. **Check multicast:**
   ```bash
   # Verify multicast routing
   ip maddr show

   # Test multicast (on sender)
   echo "test" | socat - UDP4-DATAGRAM:239.69.0.1:5353

   # Test multicast (on receiver)
   socat UDP4-RECVFROM:5353,ip-add-membership=239.69.0.1:eth0 -
   ```

3. **Check firewall:**
   ```bash
   # Allow Soluna ports
   sudo ufw allow 8400/tcp  # Control
   sudo ufw allow 5004/udp  # RTP
   sudo ufw allow 319/udp   # PTP Event
   sudo ufw allow 320/udp   # PTP General
   sudo ufw allow 5353/udp  # mDNS
   ```

4. **Check mDNS:**
   ```bash
   # Install avahi tools
   sudo apt install avahi-utils

   # Browse for Soluna services
   avahi-browse -r _soluna._tcp
   ```

### PTP Not Synchronizing

**Symptoms:** `ptp_synced: false` or large offset

**Solutions:**

1. **Check PTP traffic:**
   ```bash
   # Capture PTP packets
   sudo tcpdump -i eth0 port 319 or port 320
   ```

2. **Verify multicast membership:**
   ```bash
   netstat -g | grep 224.0.1.129
   ```

3. **Check for multiple PTP masters:**
   - Only one device should be grandmaster
   - Lower priority1 value wins

4. **Reduce network congestion:**
   - Use dedicated VLAN for audio
   - Enable QoS (DSCP EF)

### Connection Refused

**Symptoms:** Can't connect to control API

**Solutions:**

1. **Check service is running:**
   ```bash
   sudo systemctl status soluna
   ```

2. **Check port binding:**
   ```bash
   ss -tlnp | grep 8400
   ```

3. **Check firewall:**
   ```bash
   sudo ufw status
   sudo iptables -L -n | grep 8400
   ```

4. **Check bind address:**
   ```yaml
   # In config, ensure not bound to localhost only
   network:
     control_port: 8400
     # bind: 0.0.0.0  # All interfaces
   ```

## Service Issues

### Service Won't Start

**Symptoms:** `systemctl start soluna` fails

**Solutions:**

1. **Check logs:**
   ```bash
   sudo journalctl -u soluna -n 50 --no-pager
   ```

2. **Test manual start:**
   ```bash
   sudo -u soluna /usr/bin/solunad --config /etc/soluna/config.yaml
   ```

3. **Validate config:**
   ```bash
   solunad --config /etc/soluna/config.yaml --validate
   ```

4. **Check file permissions:**
   ```bash
   ls -la /etc/soluna/
   # Should be readable by soluna user
   ```

5. **Check audio device exists:**
   ```bash
   aplay -l | grep -i <device-name>
   ```

### Service Crashes

**Symptoms:** Service stops unexpectedly

**Solutions:**

1. **Check for segfaults:**
   ```bash
   dmesg | grep -i soluna
   sudo journalctl -u soluna | grep -i "signal\|crash\|fault"
   ```

2. **Enable core dumps:**
   ```bash
   # Add to service file
   [Service]
   LimitCORE=infinity

   # Reload and restart
   sudo systemctl daemon-reload
   sudo systemctl restart soluna
   ```

3. **Check memory:**
   ```bash
   free -m
   # Soluna needs ~50MB RAM minimum
   ```

4. **Run with debug logging:**
   ```yaml
   logging:
     level: "debug"
   ```

### High CPU Usage

**Symptoms:** CPU constantly at 100%

**Solutions:**

1. **Check audio parameters:**
   ```yaml
   # Reduce processing load
   audio:
     channels: 2         # Reduce channels
     frames_per_packet: 96  # Larger packets
   ```

2. **Disable unused features:**
   ```yaml
   metrics:
     enabled: false
   logging:
     level: "warn"
   ```

3. **Check for infinite loop in logs:**
   ```bash
   sudo journalctl -u soluna -f
   # Look for repeated errors
   ```

## ESP32 Issues

### ESP32 Won't Connect to WiFi

**Solutions:**

1. Verify 2.4GHz network (ESP32 doesn't support 5GHz)
2. Check SSID/password (case-sensitive)
3. Factory reset and reconfigure:
   ```
   factory_reset
   reboot
   ```
4. Check router's connected devices limit

### ESP32 Audio Glitches

**Solutions:**

1. Increase target latency:
   ```
   latency 30
   save
   ```
2. Enable FEC:
   ```
   fec 1
   save
   ```
3. Move closer to WiFi router
4. Use 5GHz band on router (fewer interferents)

### ESP32 Web UI Not Accessible

**Solutions:**

1. Check ESP32 IP address in serial console:
   ```
   status
   ```
2. Verify same network/subnet
3. Try different browser
4. Check firewall on computer

## Raspberry Pi Issues

### RPi Audio Crackling

**Solutions:**

1. Use performance CPU governor:
   ```bash
   echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
   ```

2. Disable WiFi power management:
   ```bash
   sudo iw wlan0 set power_save off
   ```

3. Increase GPU memory (reduces CPU load):
   ```
   # In /boot/config.txt
   gpu_mem=128
   ```

4. Use USB audio instead of built-in:
   ```yaml
   device:
     audio: "hw:1"  # USB device
   ```

### RPi Service Auto-restart Loop

**Solutions:**

1. Check audio device exists at boot:
   ```bash
   # Add delay to service
   sudo systemctl edit soluna

   [Service]
   ExecStartPre=/bin/sleep 5
   ```

2. Check dependencies:
   ```bash
   sudo systemctl list-dependencies soluna
   ```

## Getting Help

### Collect Diagnostic Info

```bash
# System info
uname -a
cat /etc/os-release

# Soluna version
solunad --version

# Configuration
cat /etc/soluna/config.yaml

# Logs (last 100 lines)
sudo journalctl -u soluna -n 100 --no-pager

# Audio devices
aplay -l
arecord -l

# Network
ip addr show
ip route show

# Metrics
curl http://localhost:9100/metrics 2>/dev/null | grep soluna_
```

### Reporting Issues

1. Collect diagnostic info above
2. Describe expected vs actual behavior
3. Include steps to reproduce
4. Open issue at: https://github.com/example/soluna/issues

### Community

- Discord: [soluna.dev/discord](https://soluna.dev/discord)
- Forum: [forum.soluna.dev](https://forum.soluna.dev)
- IRC: #soluna on Libera.Chat
