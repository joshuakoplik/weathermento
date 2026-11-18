#pragma once
// One-at-a-time gate for short HTTPS requests (LM REST + weather). Every TLS
// session costs ~40-50 KB of internal RAM; letting the weather fetch overlap
// LM's sign-in/poll burst can exhaust it. The long-lived LM WebSocket is not
// gated (it would hold the lock forever).
namespace netlock {
void init();                 // call once in setup(), before any task starts
void take();
void give();
struct Guard { Guard(){ take(); } ~Guard(){ give(); } };
}
