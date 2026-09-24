// See eyestore.h, and docs/EYE_FILES.md for the format.

#include "eyestore.h"

#include "config.h"
#include <Arduino.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <string.h>

// The partition is found by name and a subtype of our own choosing, so a
// table that happens to have some other data partition at that offset is
// never mistaken for one with a slot.  Must match partitions.csv.
#define SLOT_LABEL "eyes"
#define SLOT_SUBTYPE ((esp_partition_subtype_t)0x40)

static const uint8_t MAGIC[8] = {'C', 'R', 'E', 'E', 'P', 'E', 'Y', 'E'};

// ------------------------------------------------------------------ SHA-256 --
// mbedtls 2 (the Arduino core's) names these *_ret; mbedtls 3 drops the
// suffix.  Named once here so the rest of the file does not care.

#if MBEDTLS_VERSION_MAJOR >= 3
#define SHA_START mbedtls_sha256_starts
#define SHA_UPDATE mbedtls_sha256_update
#define SHA_FINISH mbedtls_sha256_finish
#define SHA_ONESHOT mbedtls_sha256
#else
#define SHA_START mbedtls_sha256_starts_ret
#define SHA_UPDATE mbedtls_sha256_update_ret
#define SHA_FINISH mbedtls_sha256_finish_ret
#define SHA_ONESHOT mbedtls_sha256_ret
#endif

// ------------------------------------------------------------------- header --

// The header as it sits in the file.  Read field by field out of the raw
// bytes rather than cast onto a struct, so padding and alignment can never
// move anything.
struct Header {
  uint16_t version;
  uint16_t headerBytes;
  uint32_t payloadBytes;
  char name[16];
  uint8_t sha[32];
};

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static uint32_t rd32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 1-15 characters of a-z0-9, starting with a letter, NUL-padded to the end
// of the field.  Strict on purpose: a name that passes can go into JSON, the
// console and the page's markup without escaping.
static bool nameValid(const char name[16]) {
  if (name[0] < 'a' || name[0] > 'z')
    return false;
  uint8_t i = 0;
  for (; i < 16 && name[i]; i++)
    if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9')))
      return false;
  if (i > EYE_FILE_NAME_MAX)
    return false; // no terminator inside the field
  for (; i < 16; i++)
    if (name[i])
      return false; // bytes after the terminator: not what the tool writes
  return true;
}

// Checks run in the order that gives the most useful complaint: a file
// that is not an eye file at all should be told so, not told its sizes are
// wrong; and a newer format may legitimately have other sizes.
static EyeLoadResult parseHeader(const uint8_t *b, Header &h) {
  if (memcmp(b, MAGIC, sizeof(MAGIC)))
    return EYE_LOAD_NOT_EYE_FILE;
  h.version = rd16(b + 8);
  h.headerBytes = rd16(b + 10);
  h.payloadBytes = rd32(b + 12);
  memcpy(h.name, b + 16, sizeof(h.name));
  memcpy(h.sha, b + 32, sizeof(h.sha));
  if (h.version != EYE_FILE_VERSION)
    return EYE_LOAD_VERSION;
  if (h.headerBytes != EYE_FILE_HEADER_BYTES ||
      h.payloadBytes != EYE_FILE_PAYLOAD_BYTES)
    return EYE_LOAD_SIZE;
  if (!nameValid(h.name))
    return EYE_LOAD_NAME;
  return EYE_LOAD_OK;
}

const char *eyeLoadResultText(EyeLoadResult r) {
  switch (r) {
  case EYE_LOAD_OK:
    return "loaded";
  case EYE_LOAD_NO_SLOT:
    return "this board has no eye slot: its partition table predates it, "
           "and one USB flash adds it";
  case EYE_LOAD_NOT_EYE_FILE:
    return "not an eye file";
  case EYE_LOAD_VERSION:
    return "an eye file in a format this firmware does not read";
  case EYE_LOAD_SIZE:
    return "wrong size for an eye file";
  case EYE_LOAD_NAME:
    return "the name in the file must be 1-15 of a-z and 0-9, from a letter";
  case EYE_LOAD_NAME_TAKEN:
    return "that name is taken, by a built-in design or a console word";
  case EYE_LOAD_HASH:
    return "damaged: the contents do not match the file's own checksum";
  case EYE_LOAD_INCOMPLETE:
    return "the upload stopped before the end of the file";
  case EYE_LOAD_FLASH:
    return "writing to flash failed";
  }
  return "unknown";
}

// --------------------------------------------------------------------- slot --

static const esp_partition_t *part = NULL;
static spi_flash_mmap_handle_t mapHandle;
static const uint8_t *mapped = NULL; // the slot, header first; NULL if unmapped
static bool loaded = false;
static char loadedName[16];

static void unmapSlot(void) {
  if (!mapped)
    return;
  spi_flash_munmap(mapHandle);
  mapped = NULL;
}

// Maps the slot and decides whether it holds a design.  Verifies the hash
// every time, not just the header: at boot that catches a slot that has
// rotted, and after a write it catches one that did not write what it was
// given.  A few milliseconds, on the ESP32's SHA hardware.
static bool mapAndVerify(void) {
  loaded = false;
  unmapSlot();
  const void *p;
  if (esp_partition_mmap(part, 0, EYE_FILE_BYTES, SPI_FLASH_MMAP_DATA, &p,
                         &mapHandle) != ESP_OK)
    return false;
  mapped = (const uint8_t *)p;

  Header h;
  if (parseHeader(mapped, h) != EYE_LOAD_OK)
    return false; // empty (erased flash reads 0xFF), or never completed
  uint8_t sum[32];
  if (SHA_ONESHOT(mapped + EYE_FILE_HEADER_BYTES, EYE_FILE_PAYLOAD_BYTES, sum,
                  0) ||
      memcmp(sum, h.sha, sizeof(sum))) {
    DEBUG_PRINTF("[eyes] slot holds '%.15s' but it fails its checksum\n",
                 h.name);
    return false;
  }
  memcpy(loadedName, h.name, sizeof(loadedName));
  loaded = true;
  return true;
}

void eyeStoreBegin(void) {
  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, SLOT_SUBTYPE,
                                  SLOT_LABEL);
  if (part && part->size < EYE_FILE_BYTES)
    part = NULL; // a slot too small to hold a file is no slot
  if (!part) {
    DEBUG_PRINTF("[eyes] no eye slot; a USB flash adds one\n");
    return;
  }
  if (mapAndVerify())
    DEBUG_PRINTF("[eyes] slot: %s\n", loadedName);
  else
    DEBUG_PRINTF("[eyes] slot: empty\n");
}

bool eyeStoreAvailable(void) { return part != NULL; }

uint32_t eyeStoreCapacity(void) { return part ? part->size : 0; }

const char *eyeStoreName(void) { return loaded ? loadedName : NULL; }

bool eyeStoreArt(EyeArt &out) {
  if (!loaded)
    return false;
  const uint8_t *p = mapped + EYE_FILE_HEADER_BYTES;
  out.sclera = (const uint16_t *)p;
  p += EYE_FILE_SCLERA_BYTES;
  out.iris = (const uint16_t *)p;
  p += EYE_FILE_IRIS_BYTES;
  out.upper = p;
  p += EYE_FILE_UPPER_BYTES;
  out.lower = p;
  p += EYE_FILE_LOWER_BYTES;
  out.polar = (const uint16_t *)p;
  return true;
}

bool eyeStoreErase(void) {
  if (!part)
    return false;
  loaded = false;
  unmapSlot();
  // The first sector is enough: the header is in it, and without a header
  // the slot is empty whatever the rest of it holds.
  return esp_partition_erase_range(part, 0, SPI_FLASH_SEC_SIZE) == ESP_OK;
}

// ------------------------------------------------------------------ writing --

static struct {
  bool active;           // between Begin and End or Abort
  bool hashing;          // the SHA context needs freeing
  EyeLoadResult result;  // the first thing that went wrong, or OK
  EyeWriteGate gate;
  uint32_t contentLength;
  uint32_t received;     // bytes of the file so far, header included
  uint8_t header[EYE_FILE_HEADER_BYTES];
  Header h;
  mbedtls_sha256_context sha;
} w;

static void endHashing(void) {
  if (w.hashing)
    mbedtls_sha256_free(&w.sha);
  w.hashing = false;
}

// The whole header is in.  Judge it, ask the gate, and only then erase.
static void onHeader(void) {
  w.result = parseHeader(w.header, w.h);
  if (w.result == EYE_LOAD_OK && w.contentLength != EYE_FILE_BYTES)
    w.result = EYE_LOAD_SIZE;
  if (w.result == EYE_LOAD_OK)
    w.result = w.gate(w.h.name);
  if (w.result != EYE_LOAD_OK)
    return; // refused before anything was touched: the old design stands

  loaded = false;
  unmapSlot();
  // Whole sectors, and only as many as a file needs.
  const uint32_t span = (EYE_FILE_BYTES + SPI_FLASH_SEC_SIZE - 1) /
                        SPI_FLASH_SEC_SIZE * SPI_FLASH_SEC_SIZE;
  if (esp_partition_erase_range(part, 0, span) != ESP_OK) {
    w.result = EYE_LOAD_FLASH;
    return;
  }
  mbedtls_sha256_init(&w.sha);
  w.hashing = true;
  if (SHA_START(&w.sha, 0))
    w.result = EYE_LOAD_FLASH;
}

void eyeStoreWriteBegin(uint32_t contentLength, EyeWriteGate gate) {
  endHashing();
  memset(&w, 0, sizeof(w));
  w.active = true;
  w.gate = gate;
  w.contentLength = contentLength;
  w.result = part ? EYE_LOAD_OK : EYE_LOAD_NO_SLOT;
}

void eyeStoreWrite(const uint8_t *data, size_t n) {
  if (!w.active)
    return;
  while (n && w.result == EYE_LOAD_OK) {
    if (w.received < EYE_FILE_HEADER_BYTES) {
      // Held back: the header is written last, so it is only in RAM for now.
      size_t take = EYE_FILE_HEADER_BYTES - w.received;
      if (take > n)
        take = n;
      memcpy(w.header + w.received, data, take);
      w.received += take;
      data += take;
      n -= take;
      if (w.received == EYE_FILE_HEADER_BYTES)
        onHeader();
      continue;
    }
    if (w.received + n > EYE_FILE_BYTES) {
      w.result = EYE_LOAD_SIZE; // more than the header promised
      break;
    }
    if (esp_partition_write(part, w.received, data, n) != ESP_OK ||
        SHA_UPDATE(&w.sha, data, n)) {
      w.result = EYE_LOAD_FLASH;
      break;
    }
    w.received += n;
    n = 0;
  }
}

EyeLoadResult eyeStoreWriteEnd(void) {
  if (!w.active)
    return EYE_LOAD_INCOMPLETE;
  w.active = false;

  if (w.result == EYE_LOAD_OK) {
    if (w.received < EYE_FILE_HEADER_BYTES)
      w.result = EYE_LOAD_NOT_EYE_FILE; // too short to be one
    else if (w.received < EYE_FILE_BYTES)
      w.result = EYE_LOAD_INCOMPLETE;
  }
  if (w.result == EYE_LOAD_OK) {
    uint8_t sum[32];
    if (SHA_FINISH(&w.sha, sum))
      w.result = EYE_LOAD_FLASH;
    else if (memcmp(sum, w.h.sha, sizeof(sum)))
      w.result = EYE_LOAD_HASH;
  }
  endHashing();

  // The commit: until these 64 bytes land, the slot reads as empty.
  if (w.result == EYE_LOAD_OK &&
      esp_partition_write(part, 0, w.header, EYE_FILE_HEADER_BYTES) != ESP_OK)
    w.result = EYE_LOAD_FLASH;
  if (w.result == EYE_LOAD_OK && !mapAndVerify())
    w.result = EYE_LOAD_FLASH;

  DEBUG_PRINTF("[eyes] upload: %s\n", eyeLoadResultText(w.result));
  return w.result;
}

void eyeStoreWriteAbort(void) {
  if (!w.active)
    return;
  w.active = false;
  endHashing();
  DEBUG_PRINTF("[eyes] upload abandoned after %u bytes\n",
               (unsigned)w.received);
}
