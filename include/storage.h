#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>

// ============================================================================
//  API PUBLIQUE
// ============================================================================
bool storage_init();
bool storage_enqueue(const String& payload);
bool storage_dequeue(String& out);
size_t storage_count();
bool storage_clear();
bool storage_removeOldest();
bool storage_peek(String &out);
size_t storage_flush(bool (*publishFn)(const String&));

#endif