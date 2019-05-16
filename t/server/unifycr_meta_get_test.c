#include <sys/types.h>

#include "metadata_suite.h"
#include "unifycr_meta.h"
#include "unifycr_metadata.h"
#include "t/lib/tap.h"

int unifycr_set_file_attribute_test(void)
{
    int rc;

    /* create dummy file attribute */
    unifycr_file_attr_t fattr = {0};

    fattr.gfid = 0xbeef;
    //fattr.gfid = 16;
    fattr.fid = 0xfeed;
    //fattr.fid = 8;
    snprintf(fattr.filename, 1024,  "/unifycr/filename/to/nowhere");
    fflush(NULL);

    rc = unifycr_set_file_attribute(&fattr);
    ok(UNIFYCR_SUCCESS == rc, "Stored file attribute");
    fflush(NULL);
    return 0;
}

int unifycr_get_file_attribute_test(void)
{
    int rc;
    unifycr_file_attr_t fattr;

    rc = unifycr_get_file_attribute(0xbeef, &fattr);
    //rc = unifycr_get_file_attribute(16, &fattr);

    ok(UNIFYCR_SUCCESS == rc &&
        0xbeef == fattr.gfid  &&
        //16 == fattr.gfid  &&
        0xfeed == fattr.fid &&
        //8 == fattr.fid &&
        (0 == strcmp(fattr.filename, "/unifycr/filename/to/nowhere")),
//        0 == fattr.file_attr,
        "Retrieve file attributes (rc = %d, gfid = 0x%02x, fid = 0x%02x)",
        rc, fattr.gfid, fattr.fid
    );
    return 0;
}


// this test is not run right now
int unifycr_get_file_extents_test(void)
{
    unifycr_keyval_t keyval[16];
    unifycr_key_t keys[16];

    int rc, num_values, num_keys, key_lens[16];

    rc = unifycr_get_file_extents(num_keys, &keys, key_lens,
                                  &num_values, &keyval);

    ok(UNIFYCR_SUCCESS == rc,
        "Retrieved file extents (rc = %d)", rc
    );
    return 0;
}


/*
 * Test setting file attributes with a parent
 */
int unify_set_file_attribute_test(void)
{
    int rc;

    /* create dummy file attribute */
    unifycr_file_attr_t fattr = {0};
    unifycr_file_attr_t fattr2 = {0};

    int parent_gfid = 0xabcd;

    fattr.gfid = 0xbeef;
    //fattr.gfid = 16;
    fattr.fid = 0xfeed;
    //fattr.fid = 8;
    snprintf(fattr.filename, 1024,  "/unifycr/filename/to/nowhere");

    rc = unify_set_file_attribute(&fattr, parent_gfid);

    fattr2.gfid = 0xbeed;
    //fattr.gfid = 16;
    fattr2.fid = 0xfeef;
    //fattr.fid = 8;
    snprintf(fattr2.filename, 1024,  "/unifycr/filename/to/nowhere");

    rc = unify_set_file_attribute(&fattr2, parent_gfid);

    ok(UNIFYCR_SUCCESS == rc,
       "Stored file attribute with secondary index" \
       "(rc = %d, parent = 0x%02x, gfid = 0x%02x, fid = 0x%02x)",
       rc, parent_gfid, fattr.gfid, fattr.fid
    );
    fflush(NULL);
    return 0;
}

int unify_get_file_attribute_test(void)
{
    int rc;

    int parent = 0xabcd;
    int num_children;

    unifycr_file_attr_t* fattr = calloc(sizeof(unifycr_file_attr_t), 2);

    rc = unify_get_child_file_attributes(parent, &num_children, &fattr);
    //rc = unifycr_get_file_attribute(16, &fattr);

    printf("num_children = %d\n", num_children);

    printf("rc = %d, parent = 0x%02x, gfid = 0x%02x, fid = 0x%02x", rc, parent, fattr[0].gfid, fattr[0].fid);

    ok(UNIFYCR_SUCCESS == rc &&
        0xbeef == fattr[0].gfid  &&
        //16 == fattr.gfid  &&
        0xfeed == fattr[0].fid &&
        //8 == fattr.fid &&
        (0 == strcmp(fattr[0].filename, "/unifycr/filename/to/nowhere")),
//        0 == fattr.file_attr,
        "Retrieve file attribute (rc = %d, parent = 0x%02x," \
        " gfid = 0x%02x, fid = 0x%02x)",
         rc, parent, fattr[0].gfid, fattr[0].fid
    );
    return 0;
}