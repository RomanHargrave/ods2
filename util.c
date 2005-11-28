/*
 *  linux/fs/ods2/util.c
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
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/locks.h>
#include <linux/blkdev.h>
#include <asm/uaccess.h>

#include "ods2.h"
#include "tparse.h"


u64 div64(u64 a, u32 b0) {
	u32 a1, a2;
	u32 long res;
	
	a1 = ((u32 *)&a)[0];
	a2 = ((u32 *)&a)[1];
	res = a1/b0 + (u64)a2 * (u64)(0xffffffff/b0) + a2 / b0 + (a2 * (0xffffffff % b0)) / b0;
	return res;
}


u32 vbn2lbn(struct super_block *sb, ODS2MAP *map, u32 vbn) {
	int			    idx = 0;
	u32			    curvbn = 1; /* VBN is 1 based - not 0 */
	
	while (map && map->s1[idx].cnt > 0 && curvbn < vbn && curvbn + map->s1[idx].cnt <= vbn) {
		curvbn += map->s1[idx].cnt;
		if (++idx > 15) {
			map = map->nxt;
			idx = 0;
		}
	}
	if (map && map->s1[idx].cnt > 0) {
		return map->s1[idx].lbn + (vbn - curvbn);
	}
	return 0;
}


u32 ino2fhlbn(struct super_block *sb, u32 ino) {
	ODS2SB			   *ods2p = (ODS2SB *)sb->u.generic_sbp;
	
	if (ino < 17) { /* the first 16 file headers are located at known locations in INDEXF.SYS */
		return le16_to_cpu(ods2p->hm2.hm2_w_ibmapsize) + le32_to_cpu(ods2p->hm2.hm2_l_ibmaplbn) + ino - 1;
	} else {
		ODS2FH		   *ods2fhp = (ODS2FH *)ods2p->indexf->u.generic_ip;
		
		return vbn2lbn(sb, ods2fhp->map, le16_to_cpu(ods2p->hm2.hm2_w_cluster) * 4 + le16_to_cpu(ods2p->hm2.hm2_w_ibmapsize) + ino);
	}
	return 0;
}

/*
  This function retreives all file mapping pointers and create a linked list so
  VBN's can be translated to LBN's.
  Note that this routine read ALL mapping pointers thus creating a catedral window
  for the file. Should there be extension headers they are all read directly not
  using iget to fetch them.
*/

ODS2MAP *getmap(struct super_block *sb, FH2DEF *fh2p) {
	FM2DEF			   *fm2p = (FM2DEF *)((short unsigned *)fh2p + fh2p->fh2_b_mpoffset);
	ODS2MAP			   *map = kmalloc(sizeof(ODS2MAP), GFP_KERNEL);
	ODS2MAP			   *mapfst = map;
	struct buffer_head	   *bh = NULL;
	int			    idx = 0;
	u8      		    mapinuse = 0;

	if (map == NULL) {
		printk("ODS2-fs kmalloc failed for getmap (1)\n");
		return NULL;
	}
	memset(map, 0, sizeof(ODS2MAP));
	do {
		mapinuse = fh2p->fh2_b_map_inuse;
		while (fm2p < (FM2DEF *)((short unsigned *)fh2p + fh2p->fh2_b_acoffset) && mapinuse > 0) {
			u32			  cnt = 0;
			u32			  lbn = 0;
			u16      		  size = 0;
			
			switch (fm2p->u1.fm1.fm2_v_format) {
			case 0: size = 1; break;
			case 1: cnt = fm2p->u1.fm1.fm2_b_count1; lbn = (fm2p->u1.fm1.fm2_v_highlbn << 16) | fm2p->u1.fm1.fm2_w_lowlbn; size = 2; break;
			case 2: cnt = fm2p->u1.fm2.fm2_v_count2; lbn = (le16_to_cpu(fm2p->u1.fm2.fm2_l_lbn2[1]) << 16) | le16_to_cpu(fm2p->u1.fm2.fm2_l_lbn2[0]); size = 3; break;
			case 3: cnt = (fm2p->u1.fm3.fm2_v_count2 << 16) | le16_to_cpu(fm2p->u1.fm3.fm2_w_lowcount); lbn = le32_to_cpu(fm2p->u1.fm3.fm2_l_lbn3); size = 4; break;
			}
			if (fm2p->u1.fm1.fm2_v_format > 0) {
				if (idx > 15) {
					if ((map->nxt = kmalloc(sizeof(ODS2MAP), GFP_KERNEL)) != NULL) {
						map = map->nxt;
						memset(map, 0, sizeof(ODS2MAP));
						idx = 0;
					} else {
						printk("ODS2-fs kmalloc failed for getmap (2)\n");
						return map;
					}
				}
				map->s1[idx].cnt = cnt + 1; /* the count is always N + 1 mapped blocks */
				map->s1[idx].lbn = lbn;
				idx++;
			}
			mapinuse -= size;
			fm2p = (FM2DEF *)((short unsigned *)(fm2p) + size);
		}
		
		/*
		  If there is an extension header we need to read all of them because
		  they could have additional mapping information.
		  
		  Note that we can not use iget to fetch the extension header because
		  it is not a valid inode for an ODS2 file. Only primary file header
		  can be used as an inode.
		*/
		
		if (fh2p->u3.s1.fid_w_ex_fidnum != 0) {
			u32			 lbn;
			
			if ((lbn = ino2fhlbn(sb, le16_to_cpu(fh2p->u3.s1.fid_w_ex_fidnum) | (fh2p->u3.s1.fid_b_ex_fidnmx << 16))) != 0) {
				fh2p = NULL;
				brelse(bh);
				if ((bh = sb_bread(sb, GETBLKNO(sb, lbn))) != NULL && bh->b_data != NULL) {
					fh2p = (FH2DEF *)(GETBLKP(sb, lbn, bh->b_data));
					fm2p = (FM2DEF *)((short unsigned *)fh2p + fh2p->fh2_b_mpoffset);
				}
			}
		} else {
			fh2p = NULL;
		}
	} while (fh2p != NULL);
	brelse(bh);
	return mapfst;
}


struct buffer_head *getfilebh(struct file *filp,  u32 vbn) {
	struct inode		   *inode = filp->f_dentry->d_inode;
	struct super_block	   *sb = inode->i_sb;
	ODS2FH			   *ods2fhp = (ODS2FH *)inode->u.generic_ip;
	ODS2FILE		   *ods2filep = (ODS2FILE *)filp->private_data;
	

	if ((vbn - 1) * 512 < inode->i_size) {
		u32		    lbn;

		if ((lbn = vbn2lbn(sb, ods2fhp->map, vbn)) > 0) {

			if (ods2filep->bhp == NULL || GETBLKNO(sb, lbn) != ods2filep->bhp->b_blocknr) {
				brelse(ods2filep->bhp);
				ods2filep->bhp = NULL;
				if ((ods2filep->bhp = sb_bread(sb, GETBLKNO(sb, lbn))) != NULL) {
					if (ods2filep->bhp->b_data != NULL) {
						ods2filep->data = GETBLKP(sb, lbn, ods2filep->bhp->b_data);
						return ods2filep->bhp;
					}
				}
			} else {
				ods2filep->data = GETBLKP(sb, lbn, ods2filep->bhp->b_data);
				return ods2filep->bhp;
			}
		}
	}
	return NULL;
}


int verify_fh(FH2DEF *fh2p, u32 ino) {
	u16      	       *p = (short unsigned *)fh2p;
	u16      		chksum = 0;

	for (; p < (short unsigned *)&(fh2p->fh2_w_checksum) ; chksum += le16_to_cpu(*p++));
	if (fh2p->fh2_b_idoffset <= fh2p->fh2_b_mpoffset &&
	    fh2p->fh2_b_mpoffset <= fh2p->fh2_b_acoffset &&
	    fh2p->fh2_b_acoffset <= fh2p->fh2_b_rsoffset &&
	    fh2p->u1.s1.fh2_b_structlevl == 2 && fh2p->u1.s1.fh2_b_structlevv >= 1 &&
	    fh2p->u2.s1.fh2_w_fid_num != 0 &&
	    ((fh2p->u2.s1.fh2_b_fid_nmx << 16) | le16_to_cpu(fh2p->u2.s1.fh2_w_fid_num)) == ino &&
	    fh2p->fh2_b_map_inuse <= (fh2p->fh2_b_acoffset - fh2p->fh2_b_mpoffset) &&
	    le16_to_cpu(fh2p->fh2_w_checksum) == chksum) {

		return 1; /* it is a valid file header */
	}
	return 0;
}


int save_raw(ARGBLK *argblk) {
	struct super_block	   *sb = (void *)argblk->arg;
	ODS2SB			   *ods2p = (void *)sb->u.generic_sbp;

	ods2p->flags.v_raw = 1;
	return 1;
}


int save_lowercase(ARGBLK *argblk) {
	struct super_block	   *sb = (void *)argblk->arg;
	ODS2SB			   *ods2p = (void *)sb->u.generic_sbp;

	ods2p->flags.v_lowercase = 1;
	return 1;
}


int save_dollar(ARGBLK *argblk) {
	struct super_block	   *sb = (void *)argblk->arg;
	ODS2SB			   *ods2p = (void *)sb->u.generic_sbp;

	ods2p->dollar = argblk->token[0];
	return 1;
}


int save_semicolon(ARGBLK *argblk) {
	struct super_block	   *sb = (void *)argblk->arg;
	ODS2SB			   *ods2p = (void *)sb->u.generic_sbp;

	ods2p->semicolon = argblk->token[0];
	return 1;
}


int save_version(ARGBLK *argblk) {
	struct super_block	   *sb = (void *)argblk->arg;
	ODS2SB			   *ods2p = (void *)sb->u.generic_sbp;

	ods2p->flags.v_version = argblk->mask;
	return 1;
}


TPARSE tpa1[];
TPARSE tpa10[];
TPARSE tpa11[];
TPARSE tpa20[];
TPARSE tpa21[];
TPARSE tpa30[];
TPARSE tpa31[];


TPARSE tpa1[] = {
  { "dollar", tpa10, NULL, 0, NULL, 0 },
  { "version", tpa20, NULL, 0, NULL, 0 },
  { "semicolon", tpa30, NULL, 0, NULL, 0 },
  { "lowercase", tpa1, save_lowercase, 0, NULL, 0 },
  { "raw", tpa1, save_raw, 0, NULL, 0 },
  { TPA_EOS, TPA_EXIT, NULL, 0, NULL, 0 },
  TPA_END
};



TPARSE tpa10[] = {
  { "=", tpa11, NULL, 0, NULL, 0 },
  TPA_END
};

TPARSE tpa11[] = {
  { TPA_ANY, tpa1, save_dollar, 0, NULL, 0 },
  TPA_END
};

TPARSE tpa20[] = {
  { "=", tpa21, NULL, 0, NULL, 0 },
  TPA_END
};

TPARSE tpa21[] = {
  { "all", tpa1, save_version, SB_M_VERSALL, NULL, 0 },
  { "highest", tpa1, save_version, SB_M_VERSHIGH, NULL, 0 },
  { "none", tpa1, save_version, SB_M_VERSNONE, NULL, 0 },
  TPA_END
};

TPARSE tpa30[] = {
  { "=", tpa31, NULL, 0, NULL, 0 },
  TPA_END
};

TPARSE tpa31[] = {
  { TPA_ANY, tpa1, save_semicolon, 0, NULL, 0 },
  TPA_END
};


int parse_options(struct super_block *sb, char *options) {
	ARGBLK                  argblk;

	argblk.str = options;
	argblk.arg = (long unsigned)sb;
	return tparse(&argblk, tpa1);
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
