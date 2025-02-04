/*
 * Copyright (C) 2006 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Access to Zip archives.
//

#define LOG_TAG "zip"

#include <utils/Log.h>
#include <ziparchive/zip_archive.h>

#include "ZipFile.h"

#include <zlib.h>

#include "zopfli/deflate.h"

#include <memory.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>
#include <inttypes.h>

using namespace android;

/*
 * Some environments require the "b", some choke on it.
 */
#define FILE_OPEN_RO        "rb"
#define FILE_OPEN_RW        "r+b"
#define FILE_OPEN_RW_CREATE "w+b"

/* should live somewhere else? */
static status_t errnoToStatus(int err)
{
    if (err == ENOENT)
        return NAME_NOT_FOUND;
    else if (err == EACCES)
        return PERMISSION_DENIED;
    else
        return UNKNOWN_ERROR;
}

/*
 * Open a file and parse its guts.
 */
status_t ZipFile::open(const char* zipFileName, int flags)
{
    bool newArchive = false;

    assert(mZipFp == NULL);     // no reopen

    if ((flags & kOpenTruncate))
        flags |= kOpenCreate;           // trunc implies create

    if ((flags & kOpenReadOnly) && (flags & kOpenReadWrite))
        return INVALID_OPERATION;       // not both
    if (!((flags & kOpenReadOnly) || (flags & kOpenReadWrite)))
        return INVALID_OPERATION;       // not neither
    if ((flags & kOpenCreate) && !(flags & kOpenReadWrite))
        return INVALID_OPERATION;       // create requires write

    if (flags & kOpenTruncate) {
        newArchive = true;
    } else {
        newArchive = (access(zipFileName, F_OK) != 0);
        if (!(flags & kOpenCreate) && newArchive) {
            /* not creating, must already exist */
            ALOGD("File %s does not exist", zipFileName);
            return NAME_NOT_FOUND;
        }
    }

    /* open the file */
    const char* openflags;
    if (flags & kOpenReadWrite) {
        if (newArchive)
            openflags = FILE_OPEN_RW_CREATE;
        else
            openflags = FILE_OPEN_RW;
    } else {
        openflags = FILE_OPEN_RO;
    }
    mZipFp = fopen(zipFileName, openflags);
    if (mZipFp == NULL) {
        int err = errno;
        ALOGD("fopen failed: %d\n", err);
        return errnoToStatus(err);
    }

    status_t result;
    if (!newArchive) {
        /*
         * Load the central directory.  If that fails, then this probably
         * isn't a Zip archive.
         */
        result = readCentralDir();
    } else {
        /*
         * Newly-created.  The EndOfCentralDir constructor actually
         * sets everything to be the way we want it (all zeroes).  We
         * set mNeedCDRewrite so that we create *something* if the
         * caller doesn't add any files.  (We could also just unlink
         * the file if it's brand new and nothing was added, but that's
         * probably doing more than we really should -- the user might
         * have a need for empty zip files.)
         */
        mNeedCDRewrite = true;
        result = OK;
    }

    if (flags & kOpenReadOnly)
        mReadOnly = true;
    else
        assert(!mReadOnly);

    return result;
}

/*
 * Return the Nth entry in the archive.
 */
android::ZipEntry* ZipFile::getEntryByIndex(int idx) const
{
    if (idx < 0 || idx >= (int) mEntries.size())
        return NULL;

    return mEntries[idx];
}

/*
 * Find an entry by name.
 */
android::ZipEntry* ZipFile::getEntryByName(const char* fileName) const
{
    /*
     * Do a stupid linear string-compare search.
     *
     * There are various ways to speed this up, especially since it's rare
     * to intermingle changes to the archive with "get by name" calls.  We
     * don't want to sort the mEntries vector itself, however, because
     * it's used to recreate the Central Directory.
     *
     * (Hash table works, parallel list of pointers in sorted order is good.)
     */
    int idx;

    for (idx = mEntries.size()-1; idx >= 0; idx--) {
        ZipEntry* pEntry = mEntries[idx];
        if (!pEntry->getDeleted() &&
            strcmp(fileName, pEntry->getFileName()) == 0)
        {
            return pEntry;
        }
    }

    return NULL;
}

/*
 * Empty the mEntries vector.
 */
void ZipFile::discardEntries(void)
{
    int count = mEntries.size();

    while (--count >= 0)
        delete mEntries[count];

    mEntries.clear();
}


/*
 * Find the central directory and read the contents.
 *
 * The fun thing about ZIP archives is that they may or may not be
 * readable from start to end.  In some cases, notably for archives
 * that were written to stdout, the only length information is in the
 * central directory at the end of the file.
 *
 * Of course, the central directory can be followed by a variable-length
 * comment field, so we have to scan through it backwards.  The comment
 * is at most 64K, plus we have 18 bytes for the end-of-central-dir stuff
 * itself, plus apparently sometimes people throw random junk on the end
 * just for the fun of it.
 *
 * This is all a little wobbly.  If the wrong value ends up in the EOCD
 * area, we're hosed.  This appears to be the way that everbody handles
 * it though, so we're in pretty good company if this fails.
 */
status_t ZipFile::readCentralDir(void)
{
    status_t result = OK;
    uint8_t* buf = NULL;
    off_t fileLength, seekStart;
    long readAmount;
    int i;

    fseek(mZipFp, 0, SEEK_END);
    fileLength = ftell(mZipFp);
    rewind(mZipFp);

    /* too small to be a ZIP archive? */
    if (fileLength < EndOfCentralDir::kEOCDLen) {
        ALOGD("Length is %ld -- too small\n", (long)fileLength);
        result = INVALID_OPERATION;
        goto bail;
    }

    buf = new uint8_t[EndOfCentralDir::kMaxEOCDSearch];
    if (buf == NULL) {
        ALOGD("Failure allocating %d bytes for EOCD search",
             EndOfCentralDir::kMaxEOCDSearch);
        result = NO_MEMORY;
        goto bail;
    }

    if (fileLength > EndOfCentralDir::kMaxEOCDSearch) {
        seekStart = fileLength - EndOfCentralDir::kMaxEOCDSearch;
        readAmount = EndOfCentralDir::kMaxEOCDSearch;
    } else {
        seekStart = 0;
        readAmount = (long) fileLength;
    }
    if (fseek(mZipFp, seekStart, SEEK_SET) != 0) {
        ALOGD("Failure seeking to end of zip at %ld", (long) seekStart);
        result = UNKNOWN_ERROR;
        goto bail;
    }

    /* read the last part of the file into the buffer */
    if (fread(buf, 1, readAmount, mZipFp) != (size_t) readAmount) {
        ALOGD("short file? wanted %ld\n", readAmount);
        result = UNKNOWN_ERROR;
        goto bail;
    }

    /* find the end-of-central-dir magic */
    for (i = readAmount - 4; i >= 0; i--) {
        if (buf[i] == 0x50 &&
            ZipEntry::getLongLE(&buf[i]) == EndOfCentralDir::kSignature)
        {
            ALOGV("+++ Found EOCD at buf+%d\n", i);
            break;
        }
    }
    if (i < 0) {
        ALOGD("EOCD not found, not Zip\n");
        result = INVALID_OPERATION;
        goto bail;
    }

    /* extract eocd values */
    result = mEOCD.readBuf(buf + i, readAmount - i);
    if (result != OK) {
        ALOGD("Failure reading %ld bytes of EOCD values", readAmount - i);
        goto bail;
    }
    //mEOCD.dump();

    if (mEOCD.mDiskNumber != 0 || mEOCD.mDiskWithCentralDir != 0 ||
        mEOCD.mNumEntries != mEOCD.mTotalNumEntries)
    {
        ALOGD("Archive spanning not supported\n");
        result = INVALID_OPERATION;
        goto bail;
    }

    /*
     * So far so good.  "mCentralDirSize" is the size in bytes of the
     * central directory, so we can just seek back that far to find it.
     * We can also seek forward mCentralDirOffset bytes from the
     * start of the file.
     *
     * We're not guaranteed to have the rest of the central dir in the
     * buffer, nor are we guaranteed that the central dir will have any
     * sort of convenient size.  We need to skip to the start of it and
     * read the header, then the other goodies.
     *
     * The only thing we really need right now is the file comment, which
     * we're hoping to preserve.
     */
    if (fseek(mZipFp, mEOCD.mCentralDirOffset, SEEK_SET) != 0) {
        ALOGD("Failure seeking to central dir offset %" PRIu32 "\n",
             mEOCD.mCentralDirOffset);
        result = UNKNOWN_ERROR;
        goto bail;
    }

    /*
     * Loop through and read the central dir entries.
     */
    ALOGV("Scanning %" PRIu16 " entries...\n", mEOCD.mTotalNumEntries);
    int entry;
    for (entry = 0; entry < mEOCD.mTotalNumEntries; entry++) {
        ZipEntry* pEntry = new ZipEntry;

        result = pEntry->initFromCDE(mZipFp);
        if (result != OK) {
            ALOGD("initFromCDE failed\n");
            delete pEntry;
            goto bail;
        }

        mEntries.add(pEntry);
    }


    /*
     * If all went well, we should now be back at the EOCD.
     */
    {
        uint8_t checkBuf[4];
        if (fread(checkBuf, 1, 4, mZipFp) != 4) {
            ALOGD("EOCD check read failed\n");
            result = INVALID_OPERATION;
            goto bail;
        }
        if (ZipEntry::getLongLE(checkBuf) != EndOfCentralDir::kSignature) {
            ALOGD("EOCD read check failed\n");
            result = UNKNOWN_ERROR;
            goto bail;
        }
        ALOGV("+++ EOCD read check passed\n");
    }

bail:
    delete[] buf;
    return result;
}


/*
 * Add a new file to the archive.
 *
 * This requires creating and populating a ZipEntry structure, and copying
 * the data into the file at the appropriate position.  The "appropriate
 * position" is the current location of the central directory, which we
 * casually overwrite (we can put it back later).
 *
 * If we were concerned about safety, we would want to make all changes
 * in a temp file and then overwrite the original after everything was
 * safely written.  Not really a concern for us.
 */
status_t ZipFile::addCommon(const char* fileName, const void* data, size_t size,
    const char* storageName, int compressionMethod, ZipEntry** ppEntry)
{
    ZipEntry* pEntry = NULL;
    status_t result = OK;
    long lfhPosn, startPosn, endPosn, uncompressedLen;
    FILE* inputFp = NULL;
    uint32_t crc;
    time_t modWhen;

    if (mReadOnly)
        return INVALID_OPERATION;

    assert(compressionMethod == ZipEntry::kCompressDeflated ||
           compressionMethod == ZipEntry::kCompressStored);

    /* make sure we're in a reasonable state */
    assert(mZipFp != NULL);
    assert(mEntries.size() == mEOCD.mTotalNumEntries);

    /* make sure it doesn't already exist */
    if (getEntryByName(storageName) != NULL)
        return ALREADY_EXISTS;

    if (!data) {
        inputFp = fopen(fileName, FILE_OPEN_RO);
        if (inputFp == NULL)
            return errnoToStatus(errno);
    }

    if (fseek(mZipFp, mEOCD.mCentralDirOffset, SEEK_SET) != 0) {
        result = UNKNOWN_ERROR;
        goto bail;
    }

    pEntry = new ZipEntry;
    pEntry->initNew(storageName, NULL);

    /*
     * From here on out, failures are more interesting.
     */
    mNeedCDRewrite = true;

    /*
     * Write the LFH, even though it's still mostly blank.  We need it
     * as a place-holder.  In theory the LFH isn't necessary, but in
     * practice some utilities demand it.
     */
    lfhPosn = ftell(mZipFp);
    pEntry->mLFH.write(mZipFp);
    startPosn = ftell(mZipFp);

    /*
     * Copy the data in, possibly compressing it as we go.
     */
    if (compressionMethod == ZipEntry::kCompressDeflated) {
        bool failed = false;
        result = compressFpToFp(mZipFp, inputFp, data, size, &crc);
        if (result != OK) {
            ALOGD("compression failed, storing\n");
            failed = true;
        } else {
            /*
             * Make sure it has compressed "enough".  This probably ought
             * to be set through an API call, but I don't expect our
             * criteria to change over time.
             */
            long src = inputFp ? ftell(inputFp) : size;
            long dst = ftell(mZipFp) - startPosn;
            if (dst + (dst / 10) > src) {
                ALOGD("insufficient compression (src=%ld dst=%ld), storing\n",
                    src, dst);
                failed = true;
            }
        }

        if (failed) {
            compressionMethod = ZipEntry::kCompressStored;
            if (inputFp) rewind(inputFp);
            fseek(mZipFp, startPosn, SEEK_SET);
            /* fall through to kCompressStored case */
        }
    }
    /* handle "no compression" request, or failed compression from above */
    if (compressionMethod == ZipEntry::kCompressStored) {
        if (inputFp) {
            result = copyFpToFp(mZipFp, inputFp, &crc);
        } else {
            result = copyDataToFp(mZipFp, data, size, &crc);
        }
        if (result != OK) {
            // don't need to truncate; happens in CDE rewrite
            ALOGD("failed copying data in\n");
            goto bail;
        }
    }

    // currently seeked to end of file
    uncompressedLen = inputFp ? ftell(inputFp) : size;

    /*
     * We could write the "Data Descriptor", but there doesn't seem to
     * be any point since we're going to go back and write the LFH.
     *
     * Update file offsets.
     */
    endPosn = ftell(mZipFp);            // seeked to end of compressed data

    /*
     * Success!  Fill out new values.
     */
    pEntry->setDataInfo(uncompressedLen, endPosn - startPosn, crc,
        compressionMethod);
    modWhen = getModTime(inputFp ? fileno(inputFp) : fileno(mZipFp));
    pEntry->setModWhen(modWhen);
    pEntry->setLFHOffset(lfhPosn);
    mEOCD.mNumEntries++;
    mEOCD.mTotalNumEntries++;
    mEOCD.mCentralDirSize = 0;      // mark invalid; set by flush()
    mEOCD.mCentralDirOffset = endPosn;

    /*
     * Go back and write the LFH.
     */
    if (fseek(mZipFp, lfhPosn, SEEK_SET) != 0) {
        result = UNKNOWN_ERROR;
        goto bail;
    }
    pEntry->mLFH.write(mZipFp);

    /*
     * Add pEntry to the list.
     */
    mEntries.add(pEntry);
    if (ppEntry != NULL)
        *ppEntry = pEntry;
    pEntry = NULL;

bail:
    if (inputFp != NULL)
        fclose(inputFp);
    delete pEntry;
    return result;
}

/*
 * Add an entry by copying it from another zip file.  If "padding" is
 * nonzero, the specified number of bytes will be added to the "extra"
 * field in the header.
 *
 * If "ppEntry" is non-NULL, a pointer to the new entry will be returned.
 */
status_t ZipFile::add(const ZipFile* pSourceZip, const ZipEntry* pSourceEntry,
    int padding, ZipEntry** ppEntry)
{
    ZipEntry* pEntry = NULL;
    status_t result;
    long lfhPosn, endPosn;

    if (mReadOnly)
        return INVALID_OPERATION;

    /* make sure we're in a reasonable state */
    assert(mZipFp != NULL);
    assert(mEntries.size() == mEOCD.mTotalNumEntries);

    if (fseek(mZipFp, mEOCD.mCentralDirOffset, SEEK_SET) != 0) {
        result = UNKNOWN_ERROR;
        goto bail;
    }

    pEntry = new ZipEntry;
    if (pEntry == NULL) {
        result = NO_MEMORY;
        goto bail;
    }

    result = pEntry->initFromExternal(pSourceEntry);
    if (result != OK)
        goto bail;
    if (padding != 0) {
        result = pEntry->addPadding(padding);
        if (result != OK)
            goto bail;
    }

    /*
     * From here on out, failures are more interesting.
     */
    mNeedCDRewrite = true;

    /*
     * Write the LFH.  Since we're not recompressing the data, we already
     * have all of the fields filled out.
     */
    lfhPosn = ftell(mZipFp);
    pEntry->mLFH.write(mZipFp);

    /*
     * Copy the data over.
     *
     * If the "has data descriptor" flag is set, we want to copy the DD
     * fields as well.  This is a fixed-size area immediately following
     * the data.
     */
    if (fseek(pSourceZip->mZipFp, pSourceEntry->getFileOffset(), SEEK_SET) != 0)
    {
        result = UNKNOWN_ERROR;
        goto bail;
    }

    off_t copyLen;
    copyLen = pSourceEntry->getCompressedLen();
    if ((pSourceEntry->mLFH.mGPBitFlag & ZipEntry::kUsesDataDescr) != 0)
        copyLen += ZipEntry::kDataDescriptorLen;

    if (copyPartialFpToFp(mZipFp, pSourceZip->mZipFp, copyLen, NULL)
        != OK)
    {
        ALOGW("copy of '%s' failed\n", pEntry->mCDE.mFileName);
        result = UNKNOWN_ERROR;
        goto bail;
    }

    /*
     * Update file offsets.
     */
    endPosn = ftell(mZipFp);

    /*
     * Success!  Fill out new values.
     */
    pEntry->setLFHOffset(lfhPosn);      // sets mCDE.mLocalHeaderRelOffset
    mEOCD.mNumEntries++;
    mEOCD.mTotalNumEntries++;
    mEOCD.mCentralDirSize = 0;      // mark invalid; set by flush()
    mEOCD.mCentralDirOffset = endPosn;

    /*
     * Add pEntry to the list.
     */
    mEntries.add(pEntry);
    if (ppEntry != NULL)
        *ppEntry = pEntry;
    pEntry = NULL;

    result = OK;

bail:
    delete pEntry;
    return result;
}

/*
 * Add an entry by copying it from another zip file, recompressing with
 * Zopfli if already compressed.
 *
 * If "ppEntry" is non-NULL, a pointer to the new entry will be returned.
 */
status_t ZipFile::addRecompress(const ZipFile* pSourceZip, const ZipEntry* pSourceEntry,
    ZipEntry** ppEntry)
{
    ZipEntry* pEntry = NULL;
    status_t result;
    long lfhPosn, startPosn, endPosn, uncompressedLen;

    if (mReadOnly)
        return INVALID_OPERATION;

    /* make sure we're in a reasonable state */
    assert(mZipFp != NULL);
    assert(mEntries.size() == mEOCD.mTotalNumEntries);

    if (fseek(mZipFp, mEOCD.mCentralDirOffset, SEEK_SET) != 0) {
        result = UNKNOWN_ERROR;
        goto bail;
    }

    pEntry = new ZipEntry;
    if (pEntry == NULL) {
        result = NO_MEMORY;
        goto bail;
    }

    result = pEntry->initFromExternal(pSourceEntry);
    if (result != OK)
        goto bail;

    /*
     * From here on out, failures are more interesting.
     */
    mNeedCDRewrite = true;

    /*
     * Write the LFH, even though it's still mostly blank.  We need it
     * as a place-holder.  In theory the LFH isn't necessary, but in
     * practice some utilities demand it.
     */
    lfhPosn = ftell(mZipFp);
    pEntry->mLFH.write(mZipFp);
    startPosn = ftell(mZipFp);

    /*
     * Copy the data over.
     *
     * If the "has data descriptor" flag is set, we want to copy the DD
     * fields as well.  This is a fixed-size area immediately following
     * the data.
     */
    if (fseek(pSourceZip->mZipFp, pSourceEntry->getFileOffset(), SEEK_SET) != 0)
    {
        result = UNKNOWN_ERROR;
        goto bail;
    }

    uncompressedLen = pSourceEntry->getUncompressedLen();

    if (pSourceEntry->isCompressed()) {
        void *buf = pSourceZip->uncompress(pSourceEntry);
        if (buf == NULL) {
            result = NO_MEMORY;
            goto bail;
        }
        long startPosn = ftell(mZipFp);
        uint32_t crc;
        if (compressFpToFp(mZipFp, NULL, buf, uncompressedLen, &crc) != OK) {
            ALOGW("recompress of '%s' failed\n", pEntry->mCDE.mFileName);
            result = UNKNOWN_ERROR;
            free(buf);
            goto bail;
        }
        long endPosn = ftell(mZipFp);
        pEntry->setDataInfo(uncompressedLen, endPosn - startPosn,
            pSourceEntry->getCRC32(), ZipEntry::kCompressDeflated);
        free(buf);
    } else {
        off_t copyLen;
        copyLen = pSourceEntry->getCompressedLen();
        if ((pSourceEntry->mLFH.mGPBitFlag & ZipEntry::kUsesDataDescr) != 0)
            copyLen += ZipEntry::kDataDescriptorLen;

        if (copyPartialFpToFp(mZipFp, pSourceZip->mZipFp, copyLen, NULL)
            != OK)
        {
            ALOGW("copy of '%s' failed\n", pEntry->mCDE.mFileName);
            result = UNKNOWN_ERROR;
            goto bail;
        }
    }

    /*
     * Update file offsets.
     */
    endPosn = ftell(mZipFp);

    /*
     * Success!  Fill out new values.
     */
    pEntry->setLFHOffset(lfhPosn);
    mEOCD.mNumEntries++;
    mEOCD.mTotalNumEntries++;
    mEOCD.mCentralDirSize = 0;      // mark invalid; set by flush()
    mEOCD.mCentralDirOffset = endPosn;

    /*
     * Go back and write the LFH.
     */
    if (fseek(mZipFp, lfhPosn, SEEK_SET) != 0) {
        result = UNKNOWN_ERROR;
        goto bail;
    }
    pEntry->mLFH.write(mZipFp);

    /*
     * Add pEntry to the list.
     */
    mEntries.add(pEntry);
    if (ppEntry != NULL)
        *ppEntry = pEntry;
    pEntry = NULL;

    result = OK;

bail:
    delete pEntry;
    return result;
}

/*
 * Copy all of the bytes in "src" to "dst".
 *
 * On exit, "srcFp" will be seeked to the end of the file, and "dstFp"
 * will be seeked immediately past the data.
 */
status_t ZipFile::copyFpToFp(FILE* dstFp, FILE* srcFp, uint32_t* pCRC32)
{
    uint8_t tmpBuf[32768];
    size_t count;

    *pCRC32 = crc32(0L, Z_NULL, 0);

    while (1) {
        count = fread(tmpBuf, 1, sizeof(tmpBuf), srcFp);
        if (ferror(srcFp) || ferror(dstFp))
            return errnoToStatus(errno);
        if (count == 0)
            break;

        *pCRC32 = crc32(*pCRC32, tmpBuf, count);

        if (fwrite(tmpBuf, 1, count, dstFp) != count) {
            ALOGD("fwrite %d bytes failed\n", (int) count);
            return UNKNOWN_ERROR;
        }
    }

    return OK;
}

/*
 * Copy all of the bytes in "src" to "dst".
 *
 * On exit, "dstFp" will be seeked immediately past the data.
 */
status_t ZipFile::copyDataToFp(FILE* dstFp,
    const void* data, size_t size, uint32_t* pCRC32)
{
    *pCRC32 = crc32(0L, Z_NULL, 0);
    if (size > 0) {
        *pCRC32 = crc32(*pCRC32, (const unsigned char*)data, size);
        if (fwrite(data, 1, size, dstFp) != size) {
            ALOGD("fwrite %d bytes failed\n", (int) size);
            return UNKNOWN_ERROR;
        }
    }

    return OK;
}

/*
 * Copy some of the bytes in "src" to "dst".
 *
 * If "pCRC32" is NULL, the CRC will not be computed.
 *
 * On exit, "srcFp" will be seeked to the end of the file, and "dstFp"
 * will be seeked immediately past the data just written.
 */
status_t ZipFile::copyPartialFpToFp(FILE* dstFp, FILE* srcFp, size_t length,
    uint32_t* pCRC32)
{
    uint8_t tmpBuf[32768];
    size_t count;

    if (pCRC32 != NULL)
        *pCRC32 = crc32(0L, Z_NULL, 0);

    while (length) {
        size_t readSize;

        readSize = sizeof(tmpBuf);
        if (readSize > length)
            readSize = length;

        count = fread(tmpBuf, 1, readSize, srcFp);
        if (count != readSize) {     // error or unexpected EOF
            ALOGD("fread %d bytes failed\n", (int) readSize);
            return UNKNOWN_ERROR;
        }

        if (pCRC32 != NULL)
            *pCRC32 = crc32(*pCRC32, tmpBuf, count);

        if (fwrite(tmpBuf, 1, count, dstFp) != count) {
            ALOGD("fwrite %d bytes failed\n", (int) count);
            return UNKNOWN_ERROR;
        }

        length -= readSize;
    }

    return OK;
}

/*
 * Compress all of the data in "srcFp" and write it to "dstFp".
 *
 * On exit, "srcFp" will be seeked to the end of the file, and "dstFp"
 * will be seeked immediately past the compressed data.
 */
status_t ZipFile::compressFpToFp(FILE* dstFp, FILE* srcFp,
    const void* data, size_t size, uint32_t* pCRC32)
{
    status_t result = OK;
    const size_t kBufSize = 1024 * 1024;
    uint8_t* inBuf = NULL;
    uint8_t* outBuf = NULL;
    size_t outSize = 0;
    bool atEof = false;     // no feof() aviailable yet
    uint32_t crc;
    ZopfliOptions options;
    unsigned char bp = 0;

    ZopfliInitOptions(&options);

    crc = crc32(0L, Z_NULL, 0);

    if (data) {
        crc = crc32(crc, (const unsigned char*)data, size);
        ZopfliDeflate(&options, 2, true, (const unsigned char*)data, size, &bp,
            &outBuf, &outSize);
    } else {
        /*
         * Create an input buffer and an output buffer.
         */
        inBuf = new uint8_t[kBufSize];
        if (inBuf == NULL) {
            result = NO_MEMORY;
            goto bail;
        }

        /*
         * Loop while we have data.
         */
        do {
            size_t getSize;
            getSize = fread(inBuf, 1, kBufSize, srcFp);
            if (ferror(srcFp)) {
                ALOGD("deflate read failed (errno=%d)\n", errno);
                result = UNKNOWN_ERROR;
                delete[] inBuf;
                goto bail;
            }
            if (getSize < kBufSize) {
                ALOGV("+++  got %d bytes, EOF reached\n",
                    (int)getSize);
                atEof = true;
            }

            crc = crc32(crc, inBuf, getSize);
            ZopfliDeflate(&options, 2, atEof, inBuf, getSize, &bp, &outBuf, &outSize);
        } while (!atEof);
        delete[] inBuf;
    }

    ALOGV("+++ writing %d bytes\n", (int)outSize);
    if (fwrite(outBuf, 1, outSize, dstFp) != outSize) {
        ALOGD("write %d failed in deflate\n", (int)outSize);
        result = UNKNOWN_ERROR;
        goto bail;
    }

    *pCRC32 = crc;

bail:
    free(outBuf);

    return result;
}

/*
 * Mark an entry as deleted.
 *
 * We will eventually need to crunch the file down, but if several files
 * are being removed (perhaps as part of an "update" process) we can make
 * things considerably faster by deferring the removal to "flush" time.
 */
status_t ZipFile::remove(ZipEntry* pEntry)
{
    /*
     * Should verify that pEntry is actually part of this archive, and
     * not some stray ZipEntry from a different file.
     */

    /* mark entry as deleted, and mark archive as dirty */
    pEntry->setDeleted();
    mNeedCDRewrite = true;
    return OK;
}

/*
 * Flush any pending writes.
 *
 * In particular, this will crunch out deleted entries, and write the
 * Central Directory and EOCD if we have stomped on them.
 */
status_t ZipFile::flush(void)
{
    status_t result = OK;
    long eocdPosn;
    int i, count;

    if (mReadOnly)
        return INVALID_OPERATION;
    if (!mNeedCDRewrite)
        return OK;

    assert(mZipFp != NULL);

    result = crunchArchive();
    if (result != OK)
        return result;

    if (fseek(mZipFp, mEOCD.mCentralDirOffset, SEEK_SET) != 0)
        return UNKNOWN_ERROR;

    count = mEntries.size();
    for (i = 0; i < count; i++) {
        ZipEntry* pEntry = mEntries[i];
        pEntry->mCDE.write(mZipFp);
    }

    eocdPosn = ftell(mZipFp);
    mEOCD.mCentralDirSize = eocdPosn - mEOCD.mCentralDirOffset;

    mEOCD.write(mZipFp);

    /*
     * If we had some stuff bloat up during compression and get replaced
     * with plain files, or if we deleted some entries, there's a lot
     * of wasted space at the end of the file.  Remove it now.
     */
    if (ftruncate(fileno(mZipFp), ftell(mZipFp)) != 0) {
        ALOGW("ftruncate failed %ld: %s\n", ftell(mZipFp), strerror(errno));
        // not fatal
    }

    /* should we clear the "newly added" flag in all entries now? */

    mNeedCDRewrite = false;
    return OK;
}

/*
 * Crunch deleted files out of an archive by shifting the later files down.
 *
 * Because we're not using a temp file, we do the operation inside the
 * current file.
 */
status_t ZipFile::crunchArchive(void)
{
    status_t result = OK;
    int i, count;
    long delCount, adjust;

#if 0
    printf("CONTENTS:\n");
    for (i = 0; i < (int) mEntries.size(); i++) {
        printf(" %d: lfhOff=%ld del=%d\n",
            i, mEntries[i]->getLFHOffset(), mEntries[i]->getDeleted());
    }
    printf("  END is %ld\n", (long) mEOCD.mCentralDirOffset);
#endif

    /*
     * Roll through the set of files, shifting them as appropriate.  We
     * could probably get a slight performance improvement by sliding
     * multiple files down at once (because we could use larger reads
     * when operating on batches of small files), but it's not that useful.
     */
    count = mEntries.size();
    delCount = adjust = 0;
    for (i = 0; i < count; i++) {
        ZipEntry* pEntry = mEntries[i];
        long span;

        if (pEntry->getLFHOffset() != 0) {
            long nextOffset;

            /* Get the length of this entry by finding the offset
             * of the next entry.  Directory entries don't have
             * file offsets, so we need to find the next non-directory
             * entry.
             */
            nextOffset = 0;
            for (int ii = i+1; nextOffset == 0 && ii < count; ii++)
                nextOffset = mEntries[ii]->getLFHOffset();
            if (nextOffset == 0)
                nextOffset = mEOCD.mCentralDirOffset;
            span = nextOffset - pEntry->getLFHOffset();

            assert(span >= ZipEntry::LocalFileHeader::kLFHLen);
        } else {
            /* This is a directory entry.  It doesn't have
             * any actual file contents, so there's no need to
             * move anything.
             */
            span = 0;
        }

        //printf("+++ %d: off=%ld span=%ld del=%d [count=%d]\n",
        //    i, pEntry->getLFHOffset(), span, pEntry->getDeleted(), count);

        if (pEntry->getDeleted()) {
            adjust += span;
            delCount++;

            delete pEntry;
            mEntries.removeAt(i);

            /* adjust loop control */
            count--;
            i--;
        } else if (span != 0 && adjust > 0) {
            /* shuffle this entry back */
            //printf("+++ Shuffling '%s' back %ld\n",
            //    pEntry->getFileName(), adjust);
            result = filemove(mZipFp, pEntry->getLFHOffset() - adjust,
                        pEntry->getLFHOffset(), span);
            if (result != OK) {
                /* this is why you use a temp file */
                ALOGE("error during crunch - archive is toast\n");
                return result;
            }

            pEntry->setLFHOffset(pEntry->getLFHOffset() - adjust);
        }
    }

    /*
     * Fix EOCD info.  We have to wait until the end to do some of this
     * because we use mCentralDirOffset to determine "span" for the
     * last entry.
     */
    mEOCD.mCentralDirOffset -= adjust;
    mEOCD.mNumEntries -= delCount;
    mEOCD.mTotalNumEntries -= delCount;
    mEOCD.mCentralDirSize = 0;  // mark invalid; set by flush()

    assert(mEOCD.mNumEntries == mEOCD.mTotalNumEntries);
    assert(mEOCD.mNumEntries == count);

    return result;
}

/*
 * Works like memmove(), but on pieces of a file.
 */
status_t ZipFile::filemove(FILE* fp, off_t dst, off_t src, size_t n)
{
    if (dst == src || n <= 0)
        return OK;

    uint8_t readBuf[32768];

    if (dst < src) {
        /* shift stuff toward start of file; must read from start */
        while (n != 0) {
            size_t getSize = sizeof(readBuf);
            if (getSize > n)
                getSize = n;

            if (fseek(fp, (long) src, SEEK_SET) != 0) {
                ALOGD("filemove src seek %ld failed\n", (long) src);
                return UNKNOWN_ERROR;
            }

            if (fread(readBuf, 1, getSize, fp) != getSize) {
                ALOGD("filemove read %ld off=%ld failed\n",
                    (long) getSize, (long) src);
                return UNKNOWN_ERROR;
            }

            if (fseek(fp, (long) dst, SEEK_SET) != 0) {
                ALOGD("filemove dst seek %ld failed\n", (long) dst);
                return UNKNOWN_ERROR;
            }

            if (fwrite(readBuf, 1, getSize, fp) != getSize) {
                ALOGD("filemove write %ld off=%ld failed\n",
                    (long) getSize, (long) dst);
                return UNKNOWN_ERROR;
            }

            src += getSize;
            dst += getSize;
            n -= getSize;
        }
    } else {
        /* shift stuff toward end of file; must read from end */
        assert(false);      // write this someday, maybe
        return UNKNOWN_ERROR;
    }

    return OK;
}


/*
 * Get the modification time from a file descriptor.
 */
time_t ZipFile::getModTime(int fd)
{
    struct stat sb;

    if (fstat(fd, &sb) < 0) {
        ALOGD("HEY: fstat on fd %d failed\n", fd);
        return (time_t) -1;
    }

    return sb.st_mtime;
}


#if 0       /* this is a bad idea */
/*
 * Get a copy of the Zip file descriptor.
 *
 * We don't allow this if the file was opened read-write because we tend
 * to leave the file contents in an uncertain state between calls to
 * flush().  The duplicated file descriptor should only be valid for reads.
 */
int ZipFile::getZipFd(void) const
{
    if (!mReadOnly)
        return INVALID_OPERATION;
    assert(mZipFp != NULL);

    int fd;
    fd = dup(fileno(mZipFp));
    if (fd < 0) {
        ALOGD("didn't work, errno=%d\n", errno);
    }

    return fd;
}
#endif


#if 0
/*
 * Expand data.
 */
bool ZipFile::uncompress(const ZipEntry* pEntry, void* buf) const
{
    return false;
}
#endif

class BufferWriter : public zip_archive::Writer {
  public:
    BufferWriter(void* buf, size_t size) : Writer(),
        buf_(reinterpret_cast<uint8_t*>(buf)), size_(size), bytes_written_(0) {}

    bool Append(uint8_t* buf, size_t buf_size) override {
        if (bytes_written_ + buf_size > size_) {
            return false;
        }

        memcpy(buf_ + bytes_written_, buf, buf_size);
        bytes_written_ += buf_size;
        return true;
    }

  private:
    uint8_t* const buf_;
    const size_t size_;
    size_t bytes_written_;
};

class FileReader : public zip_archive::Reader {
  public:
    FileReader(FILE* fp) : Reader(), fp_(fp), current_offset_(0) {
    }

    bool ReadAtOffset(uint8_t* buf, size_t len, uint32_t offset) const {
        // Data is usually requested sequentially, so this helps avoid pointless
        // fseeks every time we perform a read. There's an impedence mismatch
        // here because the original API was designed around pread and pwrite.
        if (offset != current_offset_) {
            if (fseek(fp_, offset, SEEK_SET) != 0) {
                return false;
            }

            current_offset_ = offset;
        }

        size_t read = fread(buf, 1, len, fp_);
        if (read != len) {
            return false;
        }

        current_offset_ += read;
        return true;
    }

  private:
    FILE* fp_;
    mutable uint32_t current_offset_;
};

// free the memory when you're done
void* ZipFile::uncompress(const ZipEntry* entry) const
{
    size_t unlen = entry->getUncompressedLen();
    size_t clen = entry->getCompressedLen();

    void* buf = malloc(unlen);
    if (buf == NULL) {
        return NULL;
    }

    fseek(mZipFp, 0, SEEK_SET);

    off_t offset = entry->getFileOffset();
    if (fseek(mZipFp, offset, SEEK_SET) != 0) {
        goto bail;
    }

    switch (entry->getCompressionMethod())
    {
        case ZipEntry::kCompressStored: {
            ssize_t amt = fread(buf, 1, unlen, mZipFp);
            if (amt != (ssize_t)unlen) {
                goto bail;
            }
#if 0
            printf("data...\n");
            const unsigned char* p = (unsigned char*)buf;
            const unsigned char* end = p+unlen;
            for (int i=0; i<32 && p < end; i++) {
                printf("0x%08x ", (int)(offset+(i*0x10)));
                for (int j=0; j<0x10 && p < end; j++) {
                    printf(" %02x", *p);
                    p++;
                }
                printf("\n");
            }
#endif

            }
            break;
        case ZipEntry::kCompressDeflated: {
            const FileReader reader(mZipFp);
            BufferWriter writer(buf, unlen);
            if (zip_archive::Inflate(reader, clen, unlen, &writer, nullptr) != 0) {
                goto bail;
            }
            break;
        }
        default:
            goto bail;
    }
    return buf;

bail:
    free(buf);
    return NULL;
}


/*
 * ===========================================================================
 *      ZipFile::EndOfCentralDir
 * ===========================================================================
 */

/*
 * Read the end-of-central-dir fields.
 *
 * "buf" should be positioned at the EOCD signature, and should contain
 * the entire EOCD area including the comment.
 */
status_t ZipFile::EndOfCentralDir::readBuf(const uint8_t* buf, int len)
{
    /* don't allow re-use */
    assert(mComment == NULL);

    if (len < kEOCDLen) {
        /* looks like ZIP file got truncated */
        ALOGD(" Zip EOCD: expected >= %d bytes, found %d\n",
            kEOCDLen, len);
        return INVALID_OPERATION;
    }

    /* this should probably be an assert() */
    if (ZipEntry::getLongLE(&buf[0x00]) != kSignature)
        return UNKNOWN_ERROR;

    mDiskNumber = ZipEntry::getShortLE(&buf[0x04]);
    mDiskWithCentralDir = ZipEntry::getShortLE(&buf[0x06]);
    mNumEntries = ZipEntry::getShortLE(&buf[0x08]);
    mTotalNumEntries = ZipEntry::getShortLE(&buf[0x0a]);
    mCentralDirSize = ZipEntry::getLongLE(&buf[0x0c]);
    mCentralDirOffset = ZipEntry::getLongLE(&buf[0x10]);
    mCommentLen = ZipEntry::getShortLE(&buf[0x14]);

    // TODO: validate mCentralDirOffset

    if (mCommentLen > 0) {
        if (kEOCDLen + mCommentLen > len) {
            ALOGD("EOCD(%d) + comment(%" PRIu16 ") exceeds len (%d)\n",
                kEOCDLen, mCommentLen, len);
            return UNKNOWN_ERROR;
        }
        mComment = new uint8_t[mCommentLen];
        memcpy(mComment, buf + kEOCDLen, mCommentLen);
    }

    return OK;
}

/*
 * Write an end-of-central-directory section.
 */
status_t ZipFile::EndOfCentralDir::write(FILE* fp)
{
    uint8_t buf[kEOCDLen];

    ZipEntry::putLongLE(&buf[0x00], kSignature);
    ZipEntry::putShortLE(&buf[0x04], mDiskNumber);
    ZipEntry::putShortLE(&buf[0x06], mDiskWithCentralDir);
    ZipEntry::putShortLE(&buf[0x08], mNumEntries);
    ZipEntry::putShortLE(&buf[0x0a], mTotalNumEntries);
    ZipEntry::putLongLE(&buf[0x0c], mCentralDirSize);
    ZipEntry::putLongLE(&buf[0x10], mCentralDirOffset);
    ZipEntry::putShortLE(&buf[0x14], mCommentLen);

    if (fwrite(buf, 1, kEOCDLen, fp) != kEOCDLen)
        return UNKNOWN_ERROR;
    if (mCommentLen > 0) {
        assert(mComment != NULL);
        if (fwrite(mComment, mCommentLen, 1, fp) != mCommentLen)
            return UNKNOWN_ERROR;
    }

    return OK;
}

/*
 * Dump the contents of an EndOfCentralDir object.
 */
void ZipFile::EndOfCentralDir::dump(void) const
{
    ALOGD(" EndOfCentralDir contents:\n");
    ALOGD("  diskNum=%" PRIu16 " diskWCD=%" PRIu16 " numEnt=%" PRIu16 " totalNumEnt=%" PRIu16 "\n",
        mDiskNumber, mDiskWithCentralDir, mNumEntries, mTotalNumEntries);
    ALOGD("  centDirSize=%" PRIu32 " centDirOff=%" PRIu32 " commentLen=%" PRIu32 "\n",
        mCentralDirSize, mCentralDirOffset, mCommentLen);
}

