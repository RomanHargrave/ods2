// Author. Paul Nankervis.
// Author. Roar Thron�s.

/*
 *  linux/fs/ods2/dir.c
 *
 * COPYRIGHT
 *	This file is distributed under the terms of the GNU General Public
 *	License (GPL). Copies of the GPL can be obtained from:
 *		ftp://prep.ai.mit.edu/pub/gnu/GPL
 *	Each contributing author retains all rights to their own work.
 *
 * Written 2003 by Jonas Lindholm <jlhm@usa.net>
 *
 */

#include <linux/config.h>
/*
#include <linux/module.h>
*/
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/locks.h>
#include <linux/blkdev.h>
#include <asm/uaccess.h>

#include "ods2.h"

/*
  This routine return one or more file names for a directory file.
  For an ODS2 file structure each file name can have one or more
  versions of a file, each file must be treated as a unique file.
*/

int ods2_readdir(struct file *filp, void *dirent, filldir_t filldir) {
	struct inode		   *inode = filp->f_dentry->d_inode;
	struct super_block	   *sb = inode->i_sb;
	struct buffer_head	   *bh = NULL;
	ODS2SB			   *ods2p = (ODS2SB *)sb->u.generic_sbp;
	ODS2FH			   *ods2fhp = (ODS2FH *)inode->u.generic_ip;
	ODS2FILE		   *ods2filep = (ODS2FILE *)filp->private_data;
	loff_t			    pos = filp->f_pos;
	u32			    vbn = (ods2filep->currec >> 9); /* get current VBN-1 to use */
	u32			    lbn;
	char			    cdirname[256];

	memset(cdirname, ' ', sizeof(cdirname));
	/*
	  When there are no more files to return the file position in file
	  is set to -1.
	*/
	
	if (pos == -1) return 0;
	
	/*
	  When we get called the first time for a directory file the file
	  position is set to 0. We must then return two fake entries,
	  . for the current directory and .. for the parent directory.
	*/
	
	if (pos == 0) {
		filldir(dirent, ".", 1, 0, inode->i_ino, DT_DIR);
		filldir(dirent, "..", 2, 1, ods2fhp->parent, DT_DIR);
		ods2filep->currec = 0;
		ods2filep->curbyte = 0;
		vbn = 0;
	}
	
	/*
	  As long we can translate the virtual block number, VBN, to a
	  logical block number, LBN, and read the block we continue to loop.
	*/
	
	while (vbn * 512 < inode->i_size && (lbn = vbn2lbn(sb, ods2fhp->map, vbn + 1)) > 0 &&
	       (bh = sb_bread(sb, GETBLKNO(sb, lbn))) != NULL && bh->b_data != NULL) {

		u16      	   *recp = (short unsigned *)((char *)(GETBLKP(sb, lbn, bh->b_data)) + (ods2filep->currec & 511));
		
		/*
		  For a ODS2 directory each block contains 1 to 62 directory entries.
		  Note that a directory entry can not span between two or more blocks.
		  We should be able to use the routine to read variable block size but
		  because directory file is so specific we do our own block decoding here.
		  When there are no more directory entries in the current block the record
		  length -1 is inserted as the last record.
		*/
		
		while (*recp != 65535 && *recp <= 512 && ods2filep->currec < inode->i_size) {
			DIRDEF	   *dire = (DIRDEF *)recp;
			char	    dirname[dire->u1.s1.dir_b_namecount + 1];
			
			memcpy(dirname, &dire->u1.s1.dir_t_name, dire->u1.s1.dir_b_namecount);
			dirname[dire->u1.s1.dir_b_namecount] = 0;
			if (ods2p->dollar != '$' || ods2p->flags.v_lowercase) {
				char	       *p = dirname;
				char		cnt = dire->u1.s1.dir_b_namecount;

				while (*p && cnt-- > 0) { if (*p == '$') { *p = ods2p->dollar; } if (ods2p->flags.v_lowercase) { *p = tolower(*p); } p++; }
			}
			if (ods2filep->curbyte == 0) { ods2filep->curbyte = ((dire->u1.s1.dir_b_namecount + 1) & ~1) + 6; }
			filp->f_pos = ods2filep->currec + ods2filep->curbyte;
			
			while (ods2filep->curbyte < dire->u1.s1.dir_w_size &&
			       !(ods2p->flags.v_version != SB_M_VERSALL && strlen(dirname) == strlen(cdirname) && strncmp(dirname, cdirname, strlen(dirname)) == 0)) {

				DIRDEF		     *dirv = (DIRDEF *)((char *)dire + ods2filep->curbyte);
				u32		      ino = (dirv->u1.s2.u2.s3.fid_b_nmx << 16) | le16_to_cpu(dirv->u1.s2.u2.s3.fid_w_num);
				char		      dirnamev[dire->u1.s1.dir_b_namecount + 1 + 5 + 1];
				
				if (ino != 4) { /* we must ignore 000000.DIR as it is the same as . */
					if (ods2p->flags.v_version == SB_M_VERSNONE) {
						sprintf(dirnamev, "%s", dirname);
					} else {
						sprintf(dirnamev, "%s%c%d", dirname, ods2p->semicolon, dirv->u1.s2.dir_w_version);
					}

					/*
					  We don't really know if the file is a directory by just checking
					  the file extension but it is the best we can do.
					  Should the file have extension .DIR but be a regular file the mistake
					  will be detected later on when the user try to walk down into
					  the false directory.
					*/

					if (filldir(dirent, dirnamev, strlen(dirnamev), filp->f_pos, ino, 
						    (strstr(dirnamev, (ods2p->flags.v_lowercase ? ".dir." : ".DIR")) == NULL ? DT_REG : DT_DIR))) {
						/*
						  We come here when filldir is unable to handle more entries.
						*/

						brelse(bh);
						return 0;
					}
					if (ods2p->flags.v_version != SB_M_VERSALL) { strcpy(cdirname, dirname); }
				}
				if (ods2p->flags.v_version == SB_M_VERSALL) {
					ods2filep->curbyte += 8;
					filp->f_pos += 8;
				} else {
					ods2filep->curbyte = le16_to_cpu(dire->u1.s1.dir_w_size);
					filp->f_pos += dire->u1.s1.dir_w_size;
				}
			}
			
			/*
			  When we come here there are no more versions for the file name.
			  We then reset our current byte offset and set current record offset
			  to the next directory entry.
			*/
			
			ods2filep->curbyte = 0;
			ods2filep->currec += le16_to_cpu(dire->u1.s1.dir_w_size) + 2;
			recp = (u16 *)((char *)recp + le16_to_cpu(dire->u1.s1.dir_w_size) + 2);
		}
		
		/*
		  When we come here there are no more directory entries in the current block
		  and we just release the buffer and increase the VBN counter.
		*/
		
		brelse(bh);
		vbn++;
		ods2filep->currec = vbn * 512;
	}
	filp->f_pos = -1; /* this mark that we have no more files to return */
	return 0;
}

struct _dir {
	short dir$w_size;
	short dir$w_verlimit;
	char dir$b_flags;
	char dir$b_namecount;
	char dir$t_name[1];
};

struct _dir1 {
	char dir$w_version;
	char dir$fid[3];
};

struct dsc$descriptor {
	unsigned short        dsc$w_length;   
	unsigned char dsc$b_dtype;    
	unsigned char dsc$b_class;    
	void          *dsc$a_pointer; 
};

struct _fiddef {
	short fid$w_num;
	short fid$w_seq;
	union {
		short fid$w_rvn;
		struct {
			char fid$b_rvn;
			char fid$b_nmx;
		};
	};
};

struct _fibdef {
	unsigned fib$l_acctl;
	unsigned short fib$w_fid_num;
	unsigned short fib$w_fid_seq;
	unsigned char fib$b_fid_rvn;
	unsigned char fib$b_fid_nmx;
	unsigned short fib$w_did_num;
	unsigned short fib$w_did_seq;
	unsigned char fib$b_did_rvn;
	unsigned char fib$b_did_nmx;
	unsigned fib$l_wcc;
	unsigned fib$w_nmctl;
	unsigned fib$l_exsz;
	unsigned fib$w_exctl;
	unsigned short fib$w_file_hdrseq_incr;
	unsigned short fib$w_dir_hdrseq_incr;
};

#define BLOCKSIZE 512
#define MAXREC (BLOCKSIZE - 2)

#if 0
#define STRUCT_DIR_SIZE (sizeof(struct _dir)) // but this gives one too much
#else
#define STRUCT_DIR_SIZE 7 
#endif

/* Some statistical counters... */

#define DEBUGx on
#define BLOCKSIZE 512
#define MAXREC (BLOCKSIZE - 2)

#if 0
#define STRUCT_DIR_SIZE (sizeof(struct _dir)) // but this gives one too much
#else
#define STRUCT_DIR_SIZE 7 
#endif

#define FIB$M_WILD 0x100

/* Some statistical counters... */

int direct_lookups = 0;
int direct_searches = 0;
int direct_deletes = 0;
int direct_inserts = 0;
int direct_splits = 0;
int direct_checks = 0;
int direct_matches = 0;


/* direct_show - to print directory statistics */

void direct_show(void)
{
    printk("DIRECT_SHOW Lookups: %d Searches: %d Deletes: %d Inserts: %d Splits: %d\n",
           direct_lookups,direct_searches,direct_deletes,direct_inserts,direct_splits);
}


/* name_check() - take a name specification and return name length without
               the version number, an integer version number, and a wildcard flag */

unsigned name_check(char *str,int len,int *retlen,int *retver,int *wildflag)
{
    int wildcard = 0;
    char *name_start = str;
    int dots = 0;
    char *name = name_start;
    char *name_end = name + len;
    direct_checks++;

    /* Go through the specification checking for illegal characters */

    while (name < name_end) {
        char ch = *name++;
        if (ch == '.') {
            if ((name - name_start) > 40) return SS$_BADFILENAME;
            name_start = name;
            if (dots++ > 1) break;
        } else {
            if (ch == ';') {
                break;
            } else {
                if (ch == '*' || ch == '%') {
                    wildcard = 1;
                } else {
                    if (ch == '[' || ch == ']' || ch == ':' ||
                        !isprint(ch)) return SS$_BADFILENAME;
                }
            }
        }
    }
    if ((name - name_start) > 40) return SS$_BADFILENAME;

    /* Return the name length and start checking the version */

    *retlen = name - str - 1;
    dots = 0;
    if (name < name_end) {
        char ch = *name;
        if (ch == '*') {
            if (++name < name_end) return SS$_BADFILENAME;
            dots = 32768;       /* Wildcard representation of version! */
            wildcard = 1;
        } else {
            int sign = 1;
            if (ch == '-') {
                name++;
                sign = -1;
            }
            while (name < name_end) {
                ch = *name++;
                if (!isdigit(ch)) return SS$_BADFILENAME;
                dots = dots * 10 + (ch - '0');
            }
            dots *= sign;
        }
    }
    *retver = dots;
    *wildflag = wildcard;
    return SS$_NORMAL;
}



#define MAT_LT 0
#define MAT_EQ 1
#define MAT_GT 2
#define MAT_NE 3

/* name_match() - compare a name specification with a directory entry
               and determine if there is a match, too big, too small... */

int name_match(char *spec,int spec_len,char *dirent,int dirent_len)
{
    int percent = MAT_GT;
    char *name = spec,*entry = dirent;
    char *name_end = name + spec_len,*entry_end = entry + dirent_len;
    direct_matches++;

    /* See how much name matches without wildcards... */

    while (name < name_end && entry < entry_end) {
        char sch = *name;
        if (sch != '*') {
            char ech = *entry;
                if (sch != ech) if (toupper(sch) != toupper(ech))
                    if (sch == '%') {
                        percent = MAT_NE;
                    } else {
                        break;
                    }
        } else {
            break;
        }
        name++;
        entry++;
    }

    /* Mismatch - return result unless wildcard... */

    if (name >= name_end) {
        if (entry >= entry_end) {
            return MAT_EQ;
        } else {
            return percent;
        }
    } else {

        /* See if we can find a match with wildcards */

        if (*name != '*') {
            if (percent == MAT_NE) return MAT_NE;
            if (entry < entry_end)
                if (toupper(*entry) > toupper(*name)) return MAT_GT;
            return MAT_LT;
        }
        /* Strip out wildcard(s) - if end then we match! */

        do {
            name++;
        } while (name < name_end && *name == '*');
        if (name >= name_end) return MAT_EQ;

        /* Proceed to examine the specification past the wildcard... */

        while (name < name_end) {
            int offset = 1;
            char fch = toupper(*name++);

            /* See if can can find a match for the first character... */

            if (fch != '%') {
                while (entry < entry_end) {
                    if (toupper(*entry) != fch) {
                        entry++;
                    } else {
                        break;
                    }
                }
            }
            /* Give up if we can't find that one lousy character... */

            if (entry >= entry_end) return MAT_NE;
            entry++;

            /* See how much of the rest we can match... */

            while (name < name_end && entry < entry_end) {
                char sch = *name,ech;
                if (sch == '*') break;
                    if (sch != (ech = *entry)) if (toupper(sch) != toupper(ech))
                        if (sch != '%') break;
                name++;
                entry++;
                offset++;
            }

            /* If matching died because of a wildcard we are OK... */

            if (name < name_end && *name == '*') {
                do {
                    name++;
                } while (name < name_end && *name == '*');
                if (name >= name_end) return MAT_EQ;

                /* Otherwise we finished OK or we need to try again... */

            } else {
                if (name >= name_end && entry >= entry_end) return MAT_EQ;
                name -= offset;
                entry -= offset - 1;
            }
        }
    }

    /* No more specification - match depends on remainder of entry... */

    if (entry < entry_end) return MAT_NE;
    return MAT_EQ;
}

void fid_copy(struct _fiddef *dst,struct _fiddef *src,unsigned rvn)
{
	dst->fid$w_num = VMSWORD(src->fid$w_num);
	dst->fid$w_seq = VMSWORD(src->fid$w_seq);
	if (src->fid$b_rvn == 0) {
		dst->fid$b_rvn = rvn;
	} else {
		dst->fid$b_rvn = src->fid$b_rvn;
	}
	dst->fid$b_nmx = src->fid$b_nmx;
}

/* insert_ent() - procedure to add a directory entry at record dr entry de */

unsigned insert_ent(struct inode * inode,unsigned eofblk,unsigned curblk,
                    char *buffer,
                    struct _dir * dr,struct _dir1 * de,
                    char *filename,unsigned filelen,
                    unsigned version,struct _fiddef * fid)
{
    unsigned sts = 1;
    int inuse = 0;
    struct _fh2 * head;
    struct super_block * sb = inode->i_sb;
    ODS2FH                     *ods2fhp = (ODS2FH *)inode->u.generic_ip;

    /* Compute space required... */

    int addlen = sizeof(struct _dir1);
    struct buffer_head * bh;
    struct buffer_head * bh2;
    direct_inserts++;
    if (de == NULL)
        addlen += (filelen + STRUCT_DIR_SIZE) & ~1;

    /* Compute block space in use ... */

    {
        char invalid_dr = 1;
        do {
            int sizecheck;
            struct _dir *nr = (struct _dir *) (buffer + inuse);
            if (dr == nr) invalid_dr = 0;
            sizecheck = VMSWORD(nr->dir$w_size);
            if (sizecheck == 0xffff)
                break;
            sizecheck += 2;
            inuse += sizecheck;
            sizecheck -= (nr->dir$b_namecount + STRUCT_DIR_SIZE) & ~1;
            if (inuse > MAXREC || (inuse & 1) || sizecheck <= 0 ||
                sizecheck % sizeof(struct _dir1) != 0 ) {
                return SS$_BADIRECTORY;
            }
        } while (1);

        if (invalid_dr) {
            panic("BUGCHECK invalid dr\n");
        }
        if (de != NULL) {
            if (VMSWORD(dr->dir$w_size) > MAXREC || (char *) de < dr->dir$t_name +
                dr->dir$b_namecount || (char *) de > (char *) dr + VMSWORD(dr->dir$w_size) + 2) {
                panic("BUGCHECK invalid de\n");
            }
        }
    }

    /* If not enough space free extend the directory... */

    if (addlen > MAXREC - inuse) {
        struct _dir *nr;
        unsigned keep_new = 0;
        char *newbuf;
        unsigned newblk = eofblk + 1;
        direct_splits++;
        printk("Splitting record... %d %d\n",dr,de);
        if (newblk > inode->i_blocks) {
            panic("I can't extend a directory yet!!\n");
        }
//        inode->fcb$l_highwater = 0;
	bh = sb_bread (sb, vbn2lbn(sb, ods2fhp->map, newblk));
	newbuf= bh->b_data;
        if (sts & 1) {
            while (newblk > curblk + 1) {
                char *frombuf;
		bh2 = sb_bread (sb, vbn2lbn(sb, ods2fhp->map, newblk-1));
		frombuf=bh2->b_data;
                if ((sts & 1) == 0) break;
                memcpy(newbuf,frombuf,BLOCKSIZE);;
                newbuf = frombuf;
                newblk--;
                if ((sts & 1) == 0) break;
            }
        } else {
        }
        if ((sts & 1) == 0) {
            return sts;
        }
        memset(newbuf,0,BLOCKSIZE);
        eofblk++;
	inode->i_blocks++;

        /* First find where the next record is... */

        nr = dr;
        if (VMSWORD(nr->dir$w_size) <= MAXREC)
            nr = (struct _dir *) ((char *) nr + VMSWORD(nr->dir$w_size) + 2);

        /* Can we split between records? */

        if (de == NULL || (char *) dr != buffer || VMSWORD(nr->dir$w_size) <= MAXREC) {
            struct _dir *sp = dr;
            if ((char *) dr == buffer && de != NULL) sp = nr;
            memcpy(newbuf,sp,((buffer + BLOCKSIZE) - (char *) sp));
            memset(sp,0,((buffer + BLOCKSIZE) - (char *) sp));
            sp->dir$w_size = VMSWORD(0xffff);
            if (sp == dr && (de != NULL || (char *) sp >= buffer + MAXREC - addlen)) {
                if (de != NULL)
                    de = (struct _dir1 *)
                        (newbuf + ((char *) de - (char *) sp));
                dr = (struct _dir *) (newbuf + ((char *) dr - (char *) sp));
                keep_new = 1;
            }
            /* OK, we have to split the record then.. */

        } else {
            unsigned reclen = (dr->dir$b_namecount +
                                        STRUCT_DIR_SIZE) & ~1;
            struct _dir *nbr = (struct _dir *) newbuf;
            printk("Super split %d %d\n",dr,de);
            memcpy(newbuf,buffer,reclen);
            memcpy(newbuf + reclen,de,((char *) nr - (char *) de) + 2);
            nbr->dir$w_size = VMSWORD(reclen + ((char *) nr - (char *) de) - 2);

            memset((char *) de + 2,0,((char *) nr - (char *) de));
            ((struct _dir *) de)->dir$w_size = VMSWORD(0xffff);
            dr->dir$w_size = VMSWORD(((char *) de - (char *) dr) - 2);
            if ((char *) de >= (char *) nr) {
                dr = (struct _dir *) newbuf;
                de = (struct _dir1 *) (newbuf + reclen);
                keep_new = 1;
            }
        }

        /* Need to decide which buffer we are going to keep (to write to) */

        if (keep_new) {
            curblk = newblk;
            buffer = newbuf;
        } else {
        }
        if ((sts & 1) == 0) printk("Bad status %d\n",sts);
    }
    /* After that we can just add the record or entry as appropriate... */

    if (de == NULL) {
        memmove((char *) dr + addlen,dr,BLOCKSIZE - (((char *) dr + addlen) - buffer));
        dr->dir$w_size = VMSWORD(addlen - 2);
        dr->dir$w_verlimit = 0;
        dr->dir$b_flags = 0;
        dr->dir$b_namecount = filelen;
        memcpy(dr->dir$t_name,filename,filelen);
        de = (struct _dir1 *) ((char *) dr + (addlen - sizeof(struct _dir1)));
    } else {
        dr->dir$w_size = VMSWORD(VMSWORD(dr->dir$w_size) + addlen);
        memmove((char *) de + addlen,de,BLOCKSIZE - (((char *) de + addlen) - buffer));
    }

    /* Write the entry values are we are done! */

    de->dir$w_version = VMSWORD(version);
    fid_copy(&de->dir$fid,fid,0);
    mark_inode_dirty(inode);
    return 1;
}


/* delete_ent() - delete a directory entry */

unsigned delete_ent(struct inode * inode,unsigned curblk,
                    struct _dir * dr,struct _dir1 * de,
                    char *buffer,unsigned eofblk)
{
    unsigned sts = 1;
    unsigned ent;
    struct _fh2 * head;
    struct buffer_head * bh;
    struct super_block * sb = inode->i_sb;
    ODS2FH                     *ods2fhp = (ODS2FH *)inode->u.generic_ip;
    direct_deletes++;
    ent = (VMSWORD(dr->dir$w_size) - STRUCT_DIR_SIZE
           - dr->dir$b_namecount + 3) / sizeof(struct _dir1);
    if (ent > 1) {
        char *ne = (char *) de + sizeof(struct _dir1);
        memcpy(de,ne,BLOCKSIZE - (ne - buffer));
        dr->dir$w_size = VMSWORD(VMSWORD(dr->dir$w_size) - sizeof(struct _dir1));
    } else {
        char *nr = (char *) dr + VMSWORD(dr->dir$w_size) + 2;
        if (eofblk == 1 || (char *) dr > buffer ||
            (nr <= buffer + MAXREC && (unsigned short) *nr < BLOCKSIZE)) {
            memcpy(dr,nr,BLOCKSIZE - (nr - buffer));
        } else {
            while (curblk < eofblk) {
                char *nxtbuffer;
		sts = 1;
		bh = sb_bread (sb, vbn2lbn(sb, ods2fhp->map, curblk+1));
		nxtbuffer=bh->b_data;
                if ((sts & 1) == 0) break;
                memcpy(buffer,nxtbuffer,BLOCKSIZE);
                buffer = nxtbuffer;
            }
            if (sts & 1) {
		    inode->i_blocks=eofblk;
		    eofblk--;
            }
        }
    }
    {
	    unsigned retsts = 1;
        if (sts & 1) sts = retsts;
	mark_inode_dirty(inode);
        return sts;
    }
}


/* return_ent() - return information about a directory entry */

unsigned return_ent(struct inode * inode,unsigned curblk,
                    struct _dir * dr,struct _dir1 * de,struct _fibdef * fib,
                    unsigned short *reslen,struct dsc$descriptor * resdsc,
                    int wildcard)
{
    int scale = 10;
    int version = VMSWORD(de->dir$w_version);
    int length = dr->dir$b_namecount;
    struct super_block * sb = inode->i_sb;
    ODS2FH                     *ods2fhp = (ODS2FH *)inode->u.generic_ip;
    char *ptr = resdsc->dsc$a_pointer;
    int outlen = resdsc->dsc$w_length;
    if (length > outlen) length = outlen;
    memcpy(ptr,dr->dir$t_name,length);
    while (version >= scale) scale *= 10;
    ptr += length;
    if (length < outlen) {
        *ptr++ = ';';
        length++;
        do {
            if (length >= outlen) break;
            scale /= 10;
            *ptr++ = version / scale + '0';
            version %= scale;
            length++;
        } while (scale > 1);
    }
    *reslen = length;
    fid_copy((struct _fiddef *)&fib->fib$w_fid_num,&de->dir$fid,0);
    //if (fib->fib$b_fid_rvn == 0) fib->fib$b_fid_rvn = fcb->fcb$b_fid_rvn;
    if (wildcard || (fib->fib$w_nmctl & FIB$M_WILD)) {
        fib->fib$l_wcc = curblk;
    } else {
        fib->fib$l_wcc = 0;
    }
    return 1;
}

// equivalent of FIND

/* search_ent() - search for a directory entry */

unsigned search_ent(struct inode * inode,
                    struct dsc$descriptor * fibdsc,struct dsc$descriptor * filedsc,
                    unsigned short *reslen,struct dsc$descriptor * resdsc,unsigned eofblk,unsigned action)
{
    unsigned sts=1,curblk;
    char *searchspec,*buffer;
    int searchlen,version,wildcard,wcc_flag;
    struct buffer_head * bh;
    struct super_block * sb = inode->i_sb;
    ODS2FH                     *ods2fhp = (ODS2FH *)inode->u.generic_ip;
    struct _fibdef *fib = (struct _fibdef *) fibdsc->dsc$a_pointer;
    direct_lookups++;

    printk("od2sfhp %x\n",ods2fhp);
    /* 1) Generate start block (wcc gives start point)
       2) Search for start
       3) Scan until found or too big or end   */

    curblk = fib->fib$l_wcc;
    if (curblk != 0) {
        searchspec = resdsc->dsc$a_pointer;
        sts = name_check(searchspec,*reslen,&searchlen,&version,&wildcard);
        if (action || wildcard) sts = SS$_BADFILENAME;
        wcc_flag = 1;
    } else {
        searchspec = filedsc->dsc$a_pointer;
        sts = name_check(searchspec,filedsc->dsc$w_length,&searchlen,&version,&wildcard);
        if ((action && wildcard) || (action > 1 && version < 0)) sts = SS$_BADFILENAME;
        wcc_flag = 0;
    }
    if ((sts & 1) == 0) return sts;


    /* Identify starting block...*/

    if (*searchspec == '*' || *searchspec == '%') {
        curblk = 1;
    } else {
        unsigned loblk = 1,hiblk = eofblk;
        if (curblk < 1 || curblk > eofblk) curblk = (eofblk + 1) / 2;
        while (loblk < hiblk) {
            int cmp;
            unsigned newblk;
            struct _dir *dr;
            direct_searches++;
	    sts = 1;
	    bh = sb_bread (sb, vbn2lbn(sb, ods2fhp->map, curblk));
	    buffer = bh->b_data;
            if ((sts & 1) == 0) return sts;
            dr = (struct _dir *) buffer;
            if (VMSWORD(dr->dir$w_size) > MAXREC) {
                cmp = MAT_GT;
            } else {
                cmp = name_match(searchspec,searchlen,dr->dir$t_name,dr->dir$b_namecount);
                if (cmp == MAT_EQ) {
                    if (wildcard || version < 1 || version > 32767) {
                        cmp = MAT_NE;   /* no match - want to find start */
                    } else {
                        struct _dir1 *de =
                            (struct _dir1 *) (dr->dir$t_name + ((dr->dir$b_namecount + 1) & ~1));
                        if (VMSWORD(de->dir$w_version) < version) {
                            cmp = MAT_GT;       /* too far... */
                        } else {
                            if (VMSWORD(de->dir$w_version) > version) {
                                cmp = MAT_LT;   /* further ahead... */
                            }
                        }
                    }
                }
            }
            switch (cmp) {
                case MAT_LT:
                    if (curblk == fib->fib$l_wcc) {
                        newblk = hiblk = loblk = curblk;
                    } else {
                        loblk = curblk;
                        newblk = (loblk + hiblk + 1) / 2;
                    }
                    break;
                case MAT_GT:
                case MAT_NE:
                    newblk = (loblk + curblk) / 2;
                    hiblk = curblk - 1;
                    break;
                default:
                    newblk = hiblk = loblk = curblk;
            }
            if (newblk != curblk) {
                curblk = newblk;
            }
        }
    }


    /* Now to read sequentially to find entry... */

    {
        char last_name[80];
        unsigned last_len = 0;
        int relver = 0;
        while ((sts & 1) && curblk <= eofblk) {
            struct _dir *dr;
            int cmp = MAT_LT;

            /* Access a directory block. Reset relative version if it starts
               with a record we haven't seen before... */

	    sts=1;
	    bh = sb_bread (sb, vbn2lbn(sb, ods2fhp->map, curblk));
	    buffer=bh->b_data;
	    if ((sts & 1) == 0) return sts;

            dr = (struct _dir *) buffer;
            if (last_len != dr->dir$b_namecount) {
                relver = 0;
            } else {
                if (name_match(last_name,last_len,dr->dir$t_name,last_len) != MAT_EQ) {
                    relver = 0;
                }
            }

            /* Now loop through the records seeing which match our spec... */

            do {
                char *nr = (char *) dr + VMSWORD(dr->dir$w_size) + 2;
                if (nr >= buffer + BLOCKSIZE) break;
                if (dr->dir$t_name + dr->dir$b_namecount >= nr) break;
                cmp = name_match(searchspec,searchlen,dr->dir$t_name,dr->dir$b_namecount);
                if (cmp == MAT_GT && wcc_flag) {
                    wcc_flag = 0;
                    searchspec = filedsc->dsc$a_pointer;
                    sts = name_check(searchspec,filedsc->dsc$w_length,&searchlen,&version,&wildcard);
                    if ((sts & 1) == 0) break;
                } else {
                    if (cmp == MAT_EQ) {
                        struct _dir1 *de = (struct _dir1 *) (dr->dir$t_name +
                                                                          ((dr->dir$b_namecount + 1) & ~1));
                        if (version == 0 && action == 2) {
                            version = VMSWORD(de->dir$w_version) + 1;
                            if (version > 32767) {
                                sts = SS$_BADFILENAME;
                                break;
                            }
                        }
                        /* Look at each directory entry to see
                           if it is what we want...    */

                        if ((char *) dr != buffer) relver = 0;
                        cmp = MAT_LT;
                        while ((char *) de < nr) {
                            if ((version < 1) ? (relver > version) : (version < VMSWORD(de->dir$w_version))) {
                                relver--;
                                de++;
                            } else {
                                if (version > 32767 || version == relver || version == VMSWORD(de->dir$w_version)) {
                                    cmp = MAT_EQ;
                                } else {
                                    cmp = MAT_GT;
                                }
                                if (wcc_flag == 0) {
                                    break;
                                } else {
                                    wcc_flag = 0;
                                    searchspec = filedsc->dsc$a_pointer;
                                    sts = name_check(searchspec,filedsc->dsc$w_length,&searchlen,&version,&wildcard);
                                    if ((sts & 1) == 0) break;
                                    if (name_match(searchspec,searchlen,dr->dir$t_name,
                                                   dr->dir$b_namecount) != MAT_EQ) {
                                        cmp = MAT_NE;
                                        break;
                                    }
                                    if (version < 0) {
                                        relver = -32768;
                                        cmp = MAT_GT;
                                        break;
                                    }
                                    if (cmp == MAT_EQ) {
                                        relver--;
                                        de++;
                                    }
                                    cmp = MAT_LT;
                                }
                            }
                        }
                        if ((sts & 1) == 0) break;

                        /* Decide what to do with the entry we have found... */

                        if (cmp == MAT_EQ) {
                            switch (action) {
                                case 0:
                                    return return_ent(inode,curblk,dr,de,fib,reslen,resdsc,wildcard);
                                case 1:
                                    return delete_ent(inode,curblk,dr,de,buffer,eofblk);
                                default:
                                    sts = SS$_DUPFILENAME;
                                    break;
                            }
                        } else {
                            if (cmp != MAT_NE && action == 2) {
                                return insert_ent(inode,eofblk,curblk,buffer,dr,de,
                                                  searchspec,searchlen,version,(struct _fiddef *) & fib->fib$w_fid_num);
                            }
                        }
                    }
                    /*  Finish unless we expect more... */

                    if (cmp == MAT_GT && wildcard == 0) break;

                    /* If this is the last record in the block store the name
                       so that if it continues into the next block we can get
                       the relative version right! Sigh! */

                    if (VMSWORD(((struct _dir *) nr)->dir$w_size) > MAXREC) {
                        last_len = dr->dir$b_namecount;
                        if (last_len > sizeof(last_name)) last_len = sizeof(last_name);
                        memcpy(last_name,dr->dir$t_name,last_len);
                        dr = (struct _dir *) nr;
                        break;
                    }
                    dr = (struct _dir *) nr;
                }
            } while (1);        /* dr records within block */

            /* We release the buffer ready to get the next one - unless it is the
               last one in which case we can't defer an insert any longer!! */

            if ((sts & 1) == 0 || action != 2 || (cmp != MAT_GT && curblk < eofblk)) {
		    unsigned dests = 1;
                if ((dests & 1) == 0) {
                    sts = dests;
                    break;
                }
                curblk++;
            } else {
                if (version == 0) version = 1;
                return insert_ent(inode,eofblk,curblk,buffer,dr,NULL,
                                  searchspec,searchlen,version,(struct _fiddef *) & fib->fib$w_fid_num);
            }
        }                       /* curblk blocks within file */
    }

    /* We achieved nothing! Report the failure... */

    if (sts & 1) {
        fib->fib$l_wcc = 0;
        if (wcc_flag || wildcard) {
            sts = SS$_NOMOREFILES;
        } else {
            sts = SS$_NOSUCHFILE;
        }
    }
    mark_buffer_dirty(bh);
    return sts;
}

int ods2_add_link (struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = dentry->d_parent->d_inode;
	const char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	struct page *page = NULL;
	int err=0;
	short reslen;
	char res[64];
	struct dsc$descriptor filedsc;
	struct dsc$descriptor resdsc;
	struct _fibdef  fib;
	struct dsc$descriptor fibdsc;
	filedsc.dsc$a_pointer=name;
	filedsc.dsc$w_length=namelen;
	resdsc.dsc$a_pointer=res;
	resdsc.dsc$w_length=64;
	fibdsc.dsc$a_pointer=&fib;
	fibdsc.dsc$w_length=sizeof(fib);

	fib.fib$w_fid_num = inode->i_ino;
	fib.fib$w_fid_seq = 1;

	search_ent(dentry->d_parent->d_inode,&fibdsc,&filedsc,&reslen,&resdsc,inode->i_blocks,2);

out:
	return err;
}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
