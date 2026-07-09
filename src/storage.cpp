#include <Arduino.h>
#include "storage.h"
#include "config.h"
#include <LittleFS.h>

static uint32_t nextSeq = 0; // numéro de séquence pour les fichiers (msg_0000000001.json, etc.)
static uint32_t lastFlushMs = 0;
bool storage_init()
{
    // Monte LittleFS. Le paramètre 'true' = formater si le montage échoue.
    if (!LittleFS.begin(true))
    {
        Serial.println("[STO] LittleFS mount FAILED");
        return false;
    }

    // Crée le dossier de queue s'il n'existe pas
    if (!LittleFS.exists(STORAGE_DIR_QUEUE))
    {
        LittleFS.mkdir(STORAGE_DIR_QUEUE);
        Serial.printf("[STO] Created queue dir %s\n", STORAGE_DIR_QUEUE);
    }

    // Parcourt les fichiers existants pour retrouver le plus grand numéro
    // de séquence (au cas où on reboot avec des messages déjà en attente).
    File dir = LittleFS.open(STORAGE_DIR_QUEUE);
    if (dir && dir.isDirectory())
    {
        File f = dir.openNextFile();
        while (f)
        {
            String name = String(f.name());
            // Nom attendu : msg_0000000042.json → on extrait 42
            int us = name.lastIndexOf('_');
            int dot = name.lastIndexOf('.');
            if (us != -1 && dot != -1 && dot > us)
            {
                uint32_t seq = name.substring(us + 1, dot).toInt();
                if (seq >= nextSeq)
                    nextSeq = seq + 1;
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }

    Serial.printf("[STO] LittleFS OK — %u messages pending, nextSeq=%u\n",
                  (unsigned)storage_count(), nextSeq);
    return true;
}
size_t storage_count()
{
    size_t count = 0;
    File dir = LittleFS.open(STORAGE_DIR_QUEUE);
    if (dir && dir.isDirectory())
    {
        File f = dir.openNextFile();
        while (f)
        {
            count++;
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }
    return count;
}
bool storage_enqueue(const String &payload)
{

    if (storage_count() >= STORAGE_MAX_MESSAGES)
    {
        Serial.println("[STO] Queue full — dropping oldest message");
        String discarded;
        storage_dequeue(discarded); // libère une place
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/msg_%010u.json",
             STORAGE_DIR_QUEUE, nextSeq);

    File f = LittleFS.open(path, FILE_WRITE);
    if (!f)
    {
        Serial.printf("[STO] Cannot open %s for write\n", path);
        return false;
    }

    size_t written = f.print(payload);
    f.close();

    if (written != payload.length())
    {
        Serial.printf("[STO] Partial write (%u/%u bytes)\n",
                      (unsigned)written, (unsigned)payload.length());
        LittleFS.remove(path); // écriture incomplète → on supprime
        return false;
    }

    nextSeq++;
    Serial.printf("[STO] Enqueued %s (%u bytes) — total=%u\n",
                  path, (unsigned)payload.length(), (unsigned)storage_count());
    return true;
}
bool storage_dequeue(String &out)
{
    if (!storage_peek(out))
    {
        return false;
    }
    return storage_removeOldest();
}
static String findOldestFile()
{
    String oldestName = "";
    File dir = LittleFS.open(STORAGE_DIR_QUEUE);
    if (dir && dir.isDirectory())
    {
        File f = dir.openNextFile();
        while (f)
        {
            String name = String(f.name());
            if (oldestName == "" || name < oldestName)
            {
                oldestName = name;
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }
    return oldestName;
}
bool storage_peek(String &out)
{
    String oldestName = findOldestFile();
    if (oldestName == "")
    {
        return false; // queue vide
    }

    String fullPath = String(STORAGE_DIR_QUEUE) + "/" + oldestName;
    File rf = LittleFS.open(fullPath, FILE_READ);
    if (!rf)
    {
        Serial.printf("[STO] Cannot read %s\n", fullPath.c_str());
        return false;
    }
    out = rf.readString();
    rf.close();
    return true;
}
bool storage_removeOldest()
{
    String oldestName = findOldestFile();
    if (oldestName == "")
    {
        return false; // queue vide
    }

    String fullPath = String(STORAGE_DIR_QUEUE) + "/" + oldestName;
    bool ok = LittleFS.remove(fullPath);
    if (ok)
    {
        Serial.printf("[STO] Removed %s — remaining=%u\n",
                      fullPath.c_str(), (unsigned)storage_count());
    }
    return ok;
}
// ============================================================================
//  FLUSH — rejeu non-bloquant de la queue
// ============================================================================
size_t storage_flush(bool (*publishFn)(const String &))
{
    size_t pending = storage_count();

    // Queue vide ? rien à faire
    if (pending == 0)
    {
        return 0;
    }

    // Rate limiting : un message par seconde maximum
    if (millis() - lastFlushMs < 1000)
    {
        return pending; // pas encore le moment, on attend
    }

    // Lire le plus ancien SANS le supprimer
    String payload;
    if (!storage_peek(payload))
    {
        return 0; // queue vide entre-temps
    }

    Serial.printf("[STO] Flushing 1 message (%u pending)...\n",
                  (unsigned)pending);

    // Tenter l'envoi via la fonction fournie par l'appelant
    bool sent = publishFn(payload);

    if (sent)
    {
        // Succès → on peut supprimer le message de la queue
        storage_removeOldest();
        lastFlushMs = millis();
        Serial.println("[STO] Flush OK, message removed from queue");
    }
    else
    {
        // Échec → on garde le message, on arrête le rejeu pour cette fois
        Serial.println("[STO] Flush FAILED, keeping message for retry");
        lastFlushMs = millis(); // on attend avant de réessayer
    }

    return storage_count();
}
