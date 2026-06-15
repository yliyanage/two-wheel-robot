// ble-bridge.cs  -  BLE <-> stdio tunnel for the Tumbller's BLE serial module.
//
// PowerShell 5.1 cannot handle WinRT BLE events, so this small .NET helper does
// the Bluetooth Low Energy work and exposes it as a plain stdin/stdout pipe:
//   - notifications from the module's "notify" characteristic  -> STDOUT
//   - lines read from STDIN  -> written to the module's "write" characteristic
// That lets drive-bridge.ps1 keep ALL the telemetry parsing, JSON output,
// command whitelist and HTTP endpoint - it just talks to this process instead
// of a COM port.
//
// NOTE: built with the .NET Framework 4 C# 5 compiler, whose WinRT 'await'
// projection is unreliable on this box, so we drive IAsyncOperation directly
// via its Completed callback + GetResults() (no await, no AsTask extension).
//
// Modes:
//   scan [seconds]
//   enum <mac-hex>
//   tunnel <mac-hex> <serviceUuid> <notifyUuid> <writeUuid>
//   tcp <port> <mac-hex> <serviceUuid> <notifyUuid> <writeUuid>
//       Binary-safe variant of tunnel that exposes the BLE serial link as a
//       localhost TCP server (raw bytes, no line buffering). avrdude can then
//       flash the board over BLE with: -P net:127.0.0.1:<port>. Experimental:
//       BLE throughput/latency and the lack of a DTR auto-reset make this
//       marginal; press the board's reset button when avrdude starts.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using Windows.Foundation;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

static class Program
{
    // ---- tiny WinRT async helper (no await / no projection extensions) ----
    static TResult Wait<TResult>(IAsyncOperation<TResult> op)
    {
        var done = new ManualResetEventSlim(false);
        op.Completed = delegate (IAsyncOperation<TResult> a, AsyncStatus s) { done.Set(); };
        done.Wait();
        if (op.Status != AsyncStatus.Completed)
            throw new Exception("async op status=" + op.Status);
        return op.GetResults();
    }

    static int Main(string[] args)
    {
        try
        {
            if (args.Length == 0) { Console.Error.WriteLine("usage: scan [secs] | enum <mac> | tunnel <mac> <svc> <notify> <write> | tcp <port> <mac> <svc> <notify> <write>"); return 2; }
            switch (args[0].ToLowerInvariant())
            {
                case "scan":   Scan(args.Length > 1 ? int.Parse(args[1]) : 12); return 0;
                case "enum":   Enum(ParseMac(args[1])); return 0;
                case "tunnel":
                    if (args.Length < 5) { Console.Error.WriteLine("tunnel needs <mac> <svc> <notify> <write>"); return 2; }
                    Tunnel(ParseMac(args[1]), new Guid(args[2]), new Guid(args[3]), new Guid(args[4]));
                    return 0;
                case "tcp":
                    if (args.Length < 6) { Console.Error.WriteLine("tcp needs <port> <mac> <svc> <notify> <write>"); return 2; }
                    TcpTunnel(int.Parse(args[1]), ParseMac(args[2]), new Guid(args[3]), new Guid(args[4]), new Guid(args[5]));
                    return 0;
                default: Console.Error.WriteLine("unknown mode: " + args[0]); return 2;
            }
        }
        catch (Exception ex) { Console.Error.WriteLine("ERR " + ex.Message); return 1; }
    }

    static ulong ParseMac(string s)
    {
        s = s.Replace(":", "").Replace("-", "").Trim();
        return ulong.Parse(s, NumberStyles.HexNumber);
    }

    // ---- scan ------------------------------------------------------------
    static void Scan(int seconds)
    {
        var name = new Dictionary<ulong, string>();
        var rssi = new Dictionary<ulong, int>();
        int recv = 0;
        var w = new BluetoothLEAdvertisementWatcher();
        try { w.ScanningMode = BluetoothLEScanningMode.Active; }
        catch (Exception ex) { Console.Error.WriteLine("(active mode unavailable: " + ex.Message + ")"); }
        w.Received += delegate (BluetoothLEAdvertisementWatcher s, BluetoothLEAdvertisementReceivedEventArgs e)
        {
            Interlocked.Increment(ref recv);
            lock (name)
            {
                string ln = e.Advertisement != null ? e.Advertisement.LocalName : null;
                if (!name.ContainsKey(e.BluetoothAddress) || (string.IsNullOrEmpty(name[e.BluetoothAddress]) && !string.IsNullOrEmpty(ln)))
                    name[e.BluetoothAddress] = ln;
                rssi[e.BluetoothAddress] = e.RawSignalStrengthInDBm;
            }
        };
        w.Stopped += delegate (BluetoothLEAdvertisementWatcher s, BluetoothLEAdvertisementWatcherStoppedEventArgs e)
        {
            Console.Error.WriteLine("watcher stopped: " + e.Error);
        };
        Console.Error.WriteLine("Scanning BLE for " + seconds + "s...");
        w.Start();
        Console.Error.WriteLine("watcher status after Start: " + w.Status);
        Thread.Sleep(seconds * 1000);
        w.Stop();
        lock (name)
        {
            foreach (var kv in name)
            {
                string n = string.IsNullOrEmpty(kv.Value) ? "(no name)" : kv.Value;
                Console.WriteLine(string.Format("{0:X12}\t{1}dBm\t{2}", kv.Key, rssi[kv.Key], n));
            }
        }
        Console.Error.WriteLine("scan done: " + name.Count + " device(s), " + recv + " adverts received");
    }

    // ---- enum ------------------------------------------------------------
    static void Enum(ulong addr)
    {
        var dev = Wait(BluetoothLEDevice.FromBluetoothAddressAsync(addr));
        if (dev == null) { Console.Error.WriteLine("connect failed (device not found / out of range)"); return; }
        Console.WriteLine("device: " + (string.IsNullOrEmpty(dev.Name) ? "(no name)" : dev.Name) + "  status=" + dev.ConnectionStatus);
        var sr = Wait(dev.GetGattServicesAsync(BluetoothCacheMode.Uncached));
        if (sr.Status != GattCommunicationStatus.Success) { Console.Error.WriteLine("GetGattServices: " + sr.Status); dev.Dispose(); return; }
        foreach (var svc in sr.Services)
        {
            Console.WriteLine("SERVICE " + svc.Uuid);
            GattCharacteristicsResult cr;
            try { cr = Wait(svc.GetCharacteristicsAsync(BluetoothCacheMode.Uncached)); }
            catch (Exception ex) { Console.WriteLine("  (chars error: " + ex.Message + ")"); continue; }
            if (cr.Status != GattCommunicationStatus.Success) { Console.WriteLine("  (chars status: " + cr.Status + ")"); continue; }
            foreach (var ch in cr.Characteristics)
                Console.WriteLine("  CHAR " + ch.Uuid + "  props=" + ch.CharacteristicProperties);
        }
        dev.Dispose();
        Console.Error.WriteLine("enum done");
    }

    // ---- tunnel ----------------------------------------------------------
    static GattCharacteristic _writeChar;
    static GattWriteOption _writeOpt = GattWriteOption.WriteWithoutResponse;
    static readonly ManualResetEventSlim _stop = new ManualResetEventSlim(false);

    static void Tunnel(ulong addr, Guid svcUuid, Guid notifyUuid, Guid writeUuid)
    {
        var dev = Wait(BluetoothLEDevice.FromBluetoothAddressAsync(addr));
        if (dev == null) { Console.Error.WriteLine("connect failed"); Environment.Exit(1); }
        dev.ConnectionStatusChanged += delegate (BluetoothLEDevice d, object o)
        {
            Console.Error.WriteLine("link: " + d.ConnectionStatus);
            if (d.ConnectionStatus == BluetoothConnectionStatus.Disconnected) _stop.Set();
        };

        var sr = Wait(dev.GetGattServicesAsync(BluetoothCacheMode.Uncached));
        if (sr.Status != GattCommunicationStatus.Success) { Console.Error.WriteLine("services: " + sr.Status); Environment.Exit(1); }
        GattDeviceService svc = null;
        foreach (var s in sr.Services) { if (s.Uuid == svcUuid) { svc = s; break; } }
        if (svc == null) { Console.Error.WriteLine("service not found: " + svcUuid); Environment.Exit(1); }

        var cr = Wait(svc.GetCharacteristicsAsync(BluetoothCacheMode.Uncached));
        GattCharacteristic notifyCh = null;
        foreach (var c in cr.Characteristics)
        {
            if (c.Uuid == notifyUuid) notifyCh = c;
            if (c.Uuid == writeUuid) _writeChar = c;
        }
        if (_writeChar == null) { Console.Error.WriteLine("write char not found: " + writeUuid); Environment.Exit(1); }
        if (notifyCh == null) { Console.Error.WriteLine("notify char not found: " + notifyUuid); Environment.Exit(1); }

        if ((_writeChar.CharacteristicProperties & GattCharacteristicProperties.WriteWithoutResponse) == 0 &&
            (_writeChar.CharacteristicProperties & GattCharacteristicProperties.Write) != 0)
            _writeOpt = GattWriteOption.WriteWithResponse;

        var stdout = Console.OpenStandardOutput();
        notifyCh.ValueChanged += delegate (GattCharacteristic c, GattValueChangedEventArgs e)
        {
            try
            {
                var reader = DataReader.FromBuffer(e.CharacteristicValue);
                byte[] bytes = new byte[e.CharacteristicValue.Length];
                reader.ReadBytes(bytes);
                stdout.Write(bytes, 0, bytes.Length);
                stdout.Flush();
            }
            catch { }
        };

        var cfg = Wait(notifyCh.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue.Notify));
        if (cfg != GattCommunicationStatus.Success) { Console.Error.WriteLine("subscribe failed: " + cfg); Environment.Exit(1); }
        Console.Error.WriteLine("tunnel up (write " + _writeOpt + "). notify->stdout, stdin->write.");

        var t = new Thread(StdinPump);
        t.IsBackground = true;
        t.Start();

        _stop.Wait();
        try { Wait(notifyCh.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue.None)); } catch { }
        dev.Dispose();
        Console.Error.WriteLine("tunnel closed");
    }

    static void StdinPump()
    {
        string line;
        while ((line = Console.In.ReadLine()) != null)
        {
            try
            {
                var dw = new DataWriter();
                dw.WriteString(line + "\n");
                Wait(_writeChar.WriteValueAsync(dw.DetachBuffer(), _writeOpt));
            }
            catch (Exception ex) { Console.Error.WriteLine("write err: " + ex.Message); }
        }
        _stop.Set();
    }

    // --- binary-safe TCP tunnel (for avrdude firmware upload over BLE) --------
    // Same BLE connect/subscribe as Tunnel, but instead of stdio it relays a
    // localhost TCP socket: socket bytes -> GATT write (raw, chunked); GATT
    // notify -> socket (raw). No line buffering, so STK500 binary survives.
    static NetworkStream _net;

    static void TcpTunnel(int port, ulong addr, Guid svcUuid, Guid notifyUuid, Guid writeUuid)
    {
        var dev = Wait(BluetoothLEDevice.FromBluetoothAddressAsync(addr));
        if (dev == null) { Console.Error.WriteLine("connect failed"); Environment.Exit(1); }
        dev.ConnectionStatusChanged += delegate (BluetoothLEDevice d, object o)
        {
            Console.Error.WriteLine("link: " + d.ConnectionStatus);
            if (d.ConnectionStatus == BluetoothConnectionStatus.Disconnected) _stop.Set();
        };

        var sr = Wait(dev.GetGattServicesAsync(BluetoothCacheMode.Uncached));
        if (sr.Status != GattCommunicationStatus.Success) { Console.Error.WriteLine("services: " + sr.Status); Environment.Exit(1); }
        GattDeviceService svc = null;
        foreach (var s in sr.Services) { if (s.Uuid == svcUuid) { svc = s; break; } }
        if (svc == null) { Console.Error.WriteLine("service not found: " + svcUuid); Environment.Exit(1); }

        var cr = Wait(svc.GetCharacteristicsAsync(BluetoothCacheMode.Uncached));
        GattCharacteristic notifyCh = null;
        foreach (var c in cr.Characteristics)
        {
            if (c.Uuid == notifyUuid) notifyCh = c;
            if (c.Uuid == writeUuid) _writeChar = c;
        }
        if (_writeChar == null) { Console.Error.WriteLine("write char not found: " + writeUuid); Environment.Exit(1); }
        if (notifyCh == null) { Console.Error.WriteLine("notify char not found: " + notifyUuid); Environment.Exit(1); }

        notifyCh.ValueChanged += delegate (GattCharacteristic c, GattValueChangedEventArgs e)
        {
            try
            {
                var reader = DataReader.FromBuffer(e.CharacteristicValue);
                byte[] bytes = new byte[e.CharacteristicValue.Length];
                reader.ReadBytes(bytes);
                var ns = _net;
                if (ns != null) { ns.Write(bytes, 0, bytes.Length); ns.Flush(); }
            }
            catch { }
        };

        var cfg = Wait(notifyCh.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue.Notify));
        if (cfg != GattCommunicationStatus.Success) { Console.Error.WriteLine("subscribe failed: " + cfg); Environment.Exit(1); }

        var listener = new TcpListener(IPAddress.Loopback, port);
        listener.Start();
        Console.Error.WriteLine("tcp tunnel up on 127.0.0.1:" + port + " (notify->socket, socket->write raw). Waiting for avrdude...");

        TcpClient client = listener.AcceptTcpClient();
        client.NoDelay = true;
        _net = client.GetStream();
        Console.Error.WriteLine("avrdude connected.");

        // socket -> GATT write, chunked to <=20 bytes (default ATT MTU payload).
        var buf = new byte[4096];
        try
        {
            int n;
            while ((n = _net.Read(buf, 0, buf.Length)) > 0)
            {
                int off = 0;
                while (off < n)
                {
                    int len = Math.Min(20, n - off);
                    var dw = new DataWriter();
                    var chunk = new byte[len];
                    Array.Copy(buf, off, chunk, 0, len);
                    dw.WriteBytes(chunk);
                    Wait(_writeChar.WriteValueAsync(dw.DetachBuffer(), _writeOpt));
                    off += len;
                }
            }
        }
        catch (Exception ex) { Console.Error.WriteLine("relay err: " + ex.Message); }

        _stop.Set();
        try { Wait(notifyCh.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue.None)); } catch { }
        try { listener.Stop(); } catch { }
        dev.Dispose();
        Console.Error.WriteLine("tcp tunnel closed");
    }
}

