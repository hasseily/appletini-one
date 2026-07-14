#include "updater_metadata.h"

int updater_metadata_validate(const updater_metadata_t *m)
{
    if (!m) {
        return 0;
    }
    if (m->magic != UPDATER_METADATA_MAGIC) {
        return 0;
    }
    if (m->version != UPDATER_METADATA_VERSION) {
        return 0;
    }
    return 1;
}
