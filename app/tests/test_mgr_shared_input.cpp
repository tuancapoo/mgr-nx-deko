#include <rex/input/device_assignment.h>
#include <algorithm>
#include <cassert>
#include <iostream>

int main() {
  using namespace rex::input;
  DeviceInfo joycon, bluetooth, keyboard;
  joycon.id = DeviceId(1); joycon.ordinal = 0;
  bluetooth.id = DeviceId(2); bluetooth.ordinal = 1;
  keyboard.id = DeviceId(3); keyboard.synthetic = true; keyboard.ordinal = 99;
  std::vector<DeviceId> devices;

  // Reproduce the old policy: the external pad is on unsigned guest user 1.
  SlotAssignment old_policy;
  old_policy.OnDevicesChanged({joycon, bluetooth});
  old_policy.DevicesForUser(0, devices);
  assert(devices == std::vector<DeviceId>{joycon.id});
  old_policy.DevicesForUser(1, devices);
  assert(devices == std::vector<DeviceId>{bluetooth.id});

  SharedAssignment mgr;
  mgr.OnDevicesChanged({joycon, bluetooth, keyboard});
  mgr.DevicesForUser(0, devices);
  assert((devices == std::vector<DeviceId>{joycon.id, bluetooth.id, keyboard.id}));
  for (uint32_t user : {1u, 2u, 3u, 4u, 99u}) {
    mgr.DevicesForUser(user, devices);
    assert(devices.empty());
  }

  // Disconnecting the first pad does not renumber a device's stable ordinal.
  mgr.OnDevicesChanged({bluetooth});
  mgr.DevicesForUser(0, devices);
  assert(devices == std::vector<DeviceId>{bluetooth.id});
  // A later reconnect with a new ordinal must still use the existing save user.
  bluetooth.ordinal = 7;
  mgr.OnDevicesChanged({bluetooth, joycon});
  mgr.DevicesForUser(0, devices);
  assert((devices == std::vector<DeviceId>{bluetooth.id, joycon.id}));
  mgr.OnDevicesChanged({});
  mgr.DevicesForUser(0, devices);
  assert(devices.empty());
  std::cout << "PASS: old P2 reproduction, shared P1, disconnect/reconnect, no ghost users\n";
}
