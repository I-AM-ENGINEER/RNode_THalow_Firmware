import asyncio
from bleak import BleakClient, BleakScanner

ADDRESS = "20:6E:F1:A7:52:12"

async def main():
    print("Scanning...")
    device = await BleakScanner.find_device_by_address(ADDRESS, timeout=10)
    if device is None:
        print("Device not found!")
        return
    print(f"Found: {device.name} ({device.address})")

    print("Connecting (may trigger pairing)...")
    try:
        async with BleakClient(ADDRESS, timeout=15) as client:
            print(f"Connected: {client.is_connected}")
            print(f"MTU: {client.mtu_size}")
            print("Services:")
            for svc in client.services:
                desc = svc.description or ""
                print(f"  {svc.uuid} ({desc})")
                for chr in svc.characteristics:
                    props = ",".join(chr.properties)
                    print(f"    {chr.uuid} [{props}] handle={chr.handle}")
                    for d in chr.descriptors:
                        print(f"      desc {d.uuid} handle={d.handle}")

            nus_rx = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
            nus_tx = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

            print(f"\nTrying write to NUS RX ({nus_rx})...")
            try:
                await client.write_gatt_char(nus_rx, b"hello")
                print("Write OK!")
            except Exception as e:
                print(f"Write failed: {type(e).__name__}: {e}")
    except Exception as e:
        print(f"Connection error: {type(e).__name__}: {e}")

asyncio.run(main())
