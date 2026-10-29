/*
 * Drive endurance: what the datasheet claims, what the drive itself implies,
 * and how many years are left.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include "gsm-drive-spec.h"

/*
 * Endurance ratings a maker publishes, and nothing else.
 *
 * A rating is per model *and* per capacity, and the makers' figures are not
 * linear in capacity (a WD Blue SN570 is 600 TBW at 1 TB and 900 at 2 TB; a
 * Crucial P3 is 440 TBW at 2 TB, not twice its 1 TB figure), so every row is
 * one exact model string at one capacity, with the maker's figure and the
 * document it comes from. A drive no row names has no published rating: the
 * lookup says so rather than guess from a vendor or a family. Models whose
 * maker publishes no figure (Crucial X9, WD SN850 without the X, every
 * vendor-wide family) are deliberately absent.
 *
 * A rating is a warranty limit ("the stated period or the TBW, whichever
 * comes first"), not a prediction of when the flash fails.
 *
 * Matching: `model_prefix` is compared case-insensitively against the start
 * of the model string the drive reports, and must end at a word boundary (the
 * next character is not a letter or digit), so "Samsung SSD 970 EVO 1TB"
 * can never catch the "970 EVO Plus" drive or a longer SKU. The row's
 * capacity must then agree with the drive's reported byte count to within
 * +-CAPACITY_TOLERANCE_PERCENT of the nominal size: drives report a "1 TB"
 * as 1,000,204,886,016 or 1,024,209,543,168 bytes, and a "500 GB" as
 * 500,107,862,016. Model strings other than the WD PC SN740 (read from
 * this workstation's drive) follow the makers' naming conventions and have
 * not been read from a drive of each kind; a string that differs simply
 * yields "not published".
 *
 * Sources were opened and read in October 2026; the Samsung capacity-to-
 * figure pairing follows the order of the cells in each data sheet.
 */
#define CAPACITY_TOLERANCE_PERCENT 3

static const GsmDriveSpecRow drive_spec_rows[] = {
  /* Samsung SSD 980 PRO */
  { "Samsung SSD 980 PRO 250GB", 250, 150,
    "Samsung 980 PRO data sheet Rev 2.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung-NVMe-SSD-980-PRO-Data-Sheet_Rev.2.1_10129504060695.pdf" },
  { "Samsung SSD 980 PRO 500GB", 500, 300,
    "Samsung 980 PRO data sheet Rev 2.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung-NVMe-SSD-980-PRO-Data-Sheet_Rev.2.1_10129504060695.pdf" },
  { "Samsung SSD 980 PRO 1TB", 1000, 600,
    "Samsung 980 PRO data sheet Rev 2.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung-NVMe-SSD-980-PRO-Data-Sheet_Rev.2.1_10129504060695.pdf" },
  { "Samsung SSD 980 PRO 2TB", 2000, 1200,
    "Samsung 980 PRO data sheet Rev 2.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung-NVMe-SSD-980-PRO-Data-Sheet_Rev.2.1_10129504060695.pdf" },

  /* Samsung SSD 980 */
  { "Samsung SSD 980 250GB", 250, 150,
    "Samsung 980 data sheet Rev 1.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_980_Data_Sheet_Rev.1.1.pdf" },
  { "Samsung SSD 980 500GB", 500, 300,
    "Samsung 980 data sheet Rev 1.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_980_Data_Sheet_Rev.1.1.pdf" },
  { "Samsung SSD 980 1TB", 1000, 600,
    "Samsung 980 data sheet Rev 1.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_980_Data_Sheet_Rev.1.1.pdf" },

  /* Samsung SSD 990 PRO */
  { "Samsung SSD 990 PRO 1TB", 1000, 600,
    "Samsung 990 PRO data sheet Rev 1.0",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_990_PRO_Datasheet_Rev.1.0.pdf" },
  { "Samsung SSD 990 PRO 2TB", 2000, 1200,
    "Samsung 990 PRO data sheet Rev 1.0",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_990_PRO_Datasheet_Rev.1.0.pdf" },

  /* Samsung SSD 970 EVO */
  { "Samsung SSD 970 EVO 250GB", 250, 150,
    "Samsung 970 EVO data sheet Rev 1.0",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung-NVMe-SSD-970-EVO-Data-Sheet_Rev.1.0.pdf" },
  { "Samsung SSD 970 EVO 500GB", 500, 300,
    "Samsung 970 EVO data sheet Rev 1.0",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung-NVMe-SSD-970-EVO-Data-Sheet_Rev.1.0.pdf" },
  { "Samsung SSD 970 EVO 1TB", 1000, 600,
    "Samsung 970 EVO data sheet Rev 1.0",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung-NVMe-SSD-970-EVO-Data-Sheet_Rev.1.0.pdf" },
  { "Samsung SSD 970 EVO 2TB", 2000, 1200,
    "Samsung 970 EVO data sheet Rev 1.0",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung-NVMe-SSD-970-EVO-Data-Sheet_Rev.1.0.pdf" },

  /* Samsung SSD 970 EVO Plus */
  { "Samsung SSD 970 EVO Plus 250GB", 250, 150,
    "Samsung 970 EVO Plus data sheet Rev 3.0",
    "https://image.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_970_EVO_Plus_Data_Sheet_Rev.3.0.pdf" },
  { "Samsung SSD 970 EVO Plus 500GB", 500, 300,
    "Samsung 970 EVO Plus data sheet Rev 3.0",
    "https://image.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_970_EVO_Plus_Data_Sheet_Rev.3.0.pdf" },
  { "Samsung SSD 970 EVO Plus 1TB", 1000, 600,
    "Samsung 970 EVO Plus data sheet Rev 3.0",
    "https://image.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_970_EVO_Plus_Data_Sheet_Rev.3.0.pdf" },
  { "Samsung SSD 970 EVO Plus 2TB", 2000, 1200,
    "Samsung 970 EVO Plus data sheet Rev 3.0",
    "https://image.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_970_EVO_Plus_Data_Sheet_Rev.3.0.pdf" },

  /* Samsung SSD 970 PRO */
  { "Samsung SSD 970 PRO 512GB", 512, 600,
    "Samsung 970 PRO data sheet Rev 1.0",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_970_PRO_Data_Sheet_Rev.1.0.pdf" },
  { "Samsung SSD 970 PRO 1TB", 1000, 1200,
    "Samsung 970 PRO data sheet Rev 1.0",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_NVMe_SSD_970_PRO_Data_Sheet_Rev.1.0.pdf" },

  /* Samsung SSD 870 EVO */
  { "Samsung SSD 870 EVO 250GB", 250, 150,
    "Samsung 870 EVO data sheet Rev 1.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_870_EVO_Data_Sheet_Rev1.1_230509_10129500053000.pdf" },
  { "Samsung SSD 870 EVO 500GB", 500, 300,
    "Samsung 870 EVO data sheet Rev 1.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_870_EVO_Data_Sheet_Rev1.1_230509_10129500053000.pdf" },
  { "Samsung SSD 870 EVO 1TB", 1000, 600,
    "Samsung 870 EVO data sheet Rev 1.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_870_EVO_Data_Sheet_Rev1.1_230509_10129500053000.pdf" },
  { "Samsung SSD 870 EVO 2TB", 2000, 1200,
    "Samsung 870 EVO data sheet Rev 1.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_870_EVO_Data_Sheet_Rev1.1_230509_10129500053000.pdf" },
  { "Samsung SSD 870 EVO 4TB", 4000, 2400,
    "Samsung 870 EVO data sheet Rev 1.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_870_EVO_Data_Sheet_Rev1.1_230509_10129500053000.pdf" },

  /* Samsung SSD 860 EVO (2.5 inch only) */
  { "Samsung SSD 860 EVO 250GB", 250, 150,
    "Samsung 860 EVO data sheet Rev 1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_860_EVO_Data_Sheet_Rev1.pdf" },
  { "Samsung SSD 860 EVO 500GB", 500, 300,
    "Samsung 860 EVO data sheet Rev 1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_860_EVO_Data_Sheet_Rev1.pdf" },
  { "Samsung SSD 860 EVO 1TB", 1000, 600,
    "Samsung 860 EVO data sheet Rev 1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_860_EVO_Data_Sheet_Rev1.pdf" },
  { "Samsung SSD 860 EVO 2TB", 2000, 1200,
    "Samsung 860 EVO data sheet Rev 1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_860_EVO_Data_Sheet_Rev1.pdf" },
  { "Samsung SSD 860 EVO 4TB", 4000, 2400,
    "Samsung 860 EVO data sheet Rev 1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_860_EVO_Data_Sheet_Rev1.pdf" },

  /* Samsung SSD 850 EVO */
  { "Samsung SSD 850 EVO 120GB", 120, 75,
    "Samsung 850 EVO data sheet Rev 3.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_850_EVO_Data_Sheet_Rev_3_1.pdf" },
  { "Samsung SSD 850 EVO 250GB", 250, 75,
    "Samsung 850 EVO data sheet Rev 3.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_850_EVO_Data_Sheet_Rev_3_1.pdf" },
  { "Samsung SSD 850 EVO 500GB", 500, 150,
    "Samsung 850 EVO data sheet Rev 3.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_850_EVO_Data_Sheet_Rev_3_1.pdf" },
  { "Samsung SSD 850 EVO 1TB", 1000, 150,
    "Samsung 850 EVO data sheet Rev 3.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_850_EVO_Data_Sheet_Rev_3_1.pdf" },
  { "Samsung SSD 850 EVO 2TB", 2000, 300,
    "Samsung 850 EVO data sheet Rev 3.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_850_EVO_Data_Sheet_Rev_3_1.pdf" },
  { "Samsung SSD 850 EVO 4TB", 4000, 300,
    "Samsung 850 EVO data sheet Rev 3.1",
    "https://download.semiconductor.samsung.com/resources/data-sheet/Samsung_SSD_850_EVO_Data_Sheet_Rev_3_1.pdf" },

  /* WD_BLACK SN850X */
  { "WD_BLACK SN850X 1000GB", 1000, 600,
    "WD_BLACK SN850X product page (archived)",
    "https://web.archive.org/web/2024/https://www.westerndigital.com/products/internal-drives/wd-black-sn850x-nvme-ssd" },
  { "WD_BLACK SN850X 2000GB", 2000, 1200,
    "WD_BLACK SN850X product page (archived)",
    "https://web.archive.org/web/2024/https://www.westerndigital.com/products/internal-drives/wd-black-sn850x-nvme-ssd" },

  /* WD_BLACK SN770 */
  { "WD_BLACK SN770 250GB", 250, 200,
    "WD_BLACK SN770 product page (archived)",
    "https://web.archive.org/web/2024/https://www.westerndigital.com/products/internal-drives/wd-black-sn770-nvme-ssd" },
  { "WD_BLACK SN770 500GB", 500, 300,
    "WD_BLACK SN770 product page (archived)",
    "https://web.archive.org/web/2024/https://www.westerndigital.com/products/internal-drives/wd-black-sn770-nvme-ssd" },
  { "WD_BLACK SN770 1TB", 1000, 600,
    "WD_BLACK SN770 product page (archived)",
    "https://web.archive.org/web/2024/https://www.westerndigital.com/products/internal-drives/wd-black-sn770-nvme-ssd" },
  { "WD_BLACK SN770 2TB", 2000, 1200,
    "WD_BLACK SN770 product page (archived)",
    "https://web.archive.org/web/2024/https://www.westerndigital.com/products/internal-drives/wd-black-sn770-nvme-ssd" },

  /* WD Blue SN570 */
  { "WD Blue SN570 250GB", 250, 150,
    "WD Blue SN570 product brief",
    "https://shop.sandisk.com/en-us/tools/documentRequestHandler?docPath=%2Fcontent%2Fdam%2Fdoc-library%2Fen_us%2Fassets%2Fpublic%2Fwestern-digital%2Fproduct%2Finternal-drives%2Fwd-blue-nvme-ssd%2Fproduct-brief-wd-blue-sn570-nvme-ssd.pdf" },
  { "WD Blue SN570 500GB", 500, 300,
    "WD Blue SN570 product brief",
    "https://shop.sandisk.com/en-us/tools/documentRequestHandler?docPath=%2Fcontent%2Fdam%2Fdoc-library%2Fen_us%2Fassets%2Fpublic%2Fwestern-digital%2Fproduct%2Finternal-drives%2Fwd-blue-nvme-ssd%2Fproduct-brief-wd-blue-sn570-nvme-ssd.pdf" },
  { "WD Blue SN570 1TB", 1000, 600,
    "WD Blue SN570 product brief",
    "https://shop.sandisk.com/en-us/tools/documentRequestHandler?docPath=%2Fcontent%2Fdam%2Fdoc-library%2Fen_us%2Fassets%2Fpublic%2Fwestern-digital%2Fproduct%2Finternal-drives%2Fwd-blue-nvme-ssd%2Fproduct-brief-wd-blue-sn570-nvme-ssd.pdf" },
  { "WD Blue SN570 2TB", 2000, 900,
    "WD Blue SN570 product brief",
    "https://shop.sandisk.com/en-us/tools/documentRequestHandler?docPath=%2Fcontent%2Fdam%2Fdoc-library%2Fen_us%2Fassets%2Fpublic%2Fwestern-digital%2Fproduct%2Finternal-drives%2Fwd-blue-nvme-ssd%2Fproduct-brief-wd-blue-sn570-nvme-ssd.pdf" },

  /* WD PC SN740 (2280; P = no encryption, Q = self-encrypting) */
  { "WD PC SN740 SDDPNQD-256G", 256, 200,
    "PC SN740 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn740-nvme-ssd/product-brief-pc-sn740-nvme-ssd.pdf" },
  { "WD PC SN740 SDDPNQD-512G", 512, 300,
    "PC SN740 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn740-nvme-ssd/product-brief-pc-sn740-nvme-ssd.pdf" },
  { "WD PC SN740 SDDPNQD-1T00", 1000, 400,
    "PC SN740 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn740-nvme-ssd/product-brief-pc-sn740-nvme-ssd.pdf" },
  { "WD PC SN740 SDDQNQD-256G", 256, 200,
    "PC SN740 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn740-nvme-ssd/product-brief-pc-sn740-nvme-ssd.pdf" },
  { "WD PC SN740 SDDQNQD-512G", 512, 300,
    "PC SN740 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn740-nvme-ssd/product-brief-pc-sn740-nvme-ssd.pdf" },
  { "WD PC SN740 SDDQNQD-1T00", 1000, 400,
    "PC SN740 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn740-nvme-ssd/product-brief-pc-sn740-nvme-ssd.pdf" },
  { "WD PC SN740 SDDPNQE-2T00", 2000, 500,
    "PC SN740 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn740-nvme-ssd/product-brief-pc-sn740-nvme-ssd.pdf" },
  { "WD PC SN740 SDDQNQE-2T00", 2000, 500,
    "PC SN740 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn740-nvme-ssd/product-brief-pc-sn740-nvme-ssd.pdf" },

  /* WD PC SN730 */
  { "WD PC SN730 SDBPNTY-256G", 256, 200,
    "PC SN730 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn730-nvme-ssd/product-brief-pc-sn730-nvme-ssd.pdf" },
  { "WD PC SN730 SDBPNTY-512G", 512, 300,
    "PC SN730 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn730-nvme-ssd/product-brief-pc-sn730-nvme-ssd.pdf" },
  { "WD PC SN730 SDBPNTY-1T00", 1000, 400,
    "PC SN730 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn730-nvme-ssd/product-brief-pc-sn730-nvme-ssd.pdf" },
  { "WD PC SN730 SDBQNTY-256G", 256, 200,
    "PC SN730 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn730-nvme-ssd/product-brief-pc-sn730-nvme-ssd.pdf" },
  { "WD PC SN730 SDBQNTY-512G", 512, 300,
    "PC SN730 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn730-nvme-ssd/product-brief-pc-sn730-nvme-ssd.pdf" },
  { "WD PC SN730 SDBQNTY-1T00", 1000, 400,
    "PC SN730 product brief",
    "https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/western-digital/product/internal-drives/pc-sn730-nvme-ssd/product-brief-pc-sn730-nvme-ssd.pdf" },

  /* Crucial MX500 (2.5 inch SSD1 SKUs); reseller-hosted copy of the maker's flyer */
  { "CT250MX500SSD1", 250, 100,
    "Crucial MX500 product flyer",
    "https://www.simms.co.uk/PDFS/Products/SSD/crucial-mx500-ssd-NEW.pdf" },
  { "CT500MX500SSD1", 500, 180,
    "Crucial MX500 product flyer",
    "https://www.simms.co.uk/PDFS/Products/SSD/crucial-mx500-ssd-NEW.pdf" },
  { "CT1000MX500SSD1", 1000, 360,
    "Crucial MX500 product flyer",
    "https://www.simms.co.uk/PDFS/Products/SSD/crucial-mx500-ssd-NEW.pdf" },
  { "CT2000MX500SSD1", 2000, 700,
    "Crucial MX500 product flyer",
    "https://www.simms.co.uk/PDFS/Products/SSD/crucial-mx500-ssd-NEW.pdf" },

  /* Crucial P3 CT500P3SSD8 */
  { "CT500P3SSD8", 500, 110,
    "Crucial P3 product page",
    "https://www.crucial.com/ssd/p3/ct500p3ssd8" },

  /* Crucial P3 CT1000P3SSD8 */
  { "CT1000P3SSD8", 1000, 220,
    "Crucial P3 product page",
    "https://www.crucial.com/ssd/p3/ct1000p3ssd8" },

  /* Crucial P3 CT2000P3SSD8 */
  { "CT2000P3SSD8", 2000, 440,
    "Crucial P3 product page",
    "https://www.crucial.com/ssd/p3/ct2000p3ssd8" },

  /* Crucial P3 CT4000P3SSD8 */
  { "CT4000P3SSD8", 4000, 800,
    "Crucial P3 product page",
    "https://www.crucial.com/ssd/p3/ct4000p3ssd8" },

  /* Crucial P3 Plus CT500P3PSSD8 */
  { "CT500P3PSSD8", 500, 110,
    "Crucial P3 Plus product page",
    "https://www.crucial.com/ssd/p3-plus/ct500p3pssd8" },

  /* Crucial P3 Plus CT1000P3PSSD8 */
  { "CT1000P3PSSD8", 1000, 220,
    "Crucial P3 Plus product page",
    "https://www.crucial.com/ssd/p3-plus/ct1000p3pssd8" },

  /* Crucial P3 Plus CT2000P3PSSD8 */
  { "CT2000P3PSSD8", 2000, 440,
    "Crucial P3 Plus product page",
    "https://www.crucial.com/ssd/p3-plus/ct2000p3pssd8" },

  /* Crucial P3 Plus CT4000P3PSSD8 */
  { "CT4000P3PSSD8", 4000, 800,
    "Crucial P3 Plus product page",
    "https://www.crucial.com/ssd/p3-plus/ct4000p3pssd8" },

  /* Kingston A2000 */
  { "KINGSTON SA2000M8250G", 250, 150,
    "Kingston A2000 data sheet",
    "https://www.kingston.com/datasheets/SA2000_en.pdf" },
  { "KINGSTON SA2000M8500G", 500, 350,
    "Kingston A2000 data sheet",
    "https://www.kingston.com/datasheets/SA2000_en.pdf" },
  { "KINGSTON SA2000M81000G", 1000, 600,
    "Kingston A2000 data sheet",
    "https://www.kingston.com/datasheets/SA2000_en.pdf" },

  /* Kingston NV2 */
  { "KINGSTON SNV2S250G", 250, 80,
    "Kingston NV2 data sheet",
    "https://www.kingston.com/datasheets/snv2s_en.pdf" },
  { "KINGSTON SNV2S500G", 500, 160,
    "Kingston NV2 data sheet",
    "https://www.kingston.com/datasheets/snv2s_en.pdf" },
  { "KINGSTON SNV2S1000G", 1000, 320,
    "Kingston NV2 data sheet",
    "https://www.kingston.com/datasheets/snv2s_en.pdf" },
  { "KINGSTON SNV2S2000G", 2000, 640,
    "Kingston NV2 data sheet",
    "https://www.kingston.com/datasheets/snv2s_en.pdf" },
  { "KINGSTON SNV2S4000G", 4000, 1280,
    "Kingston NV2 data sheet",
    "https://www.kingston.com/datasheets/snv2s_en.pdf" },

  /* Intel 660p (the 665p shares the SSDPEKNW stem and is not listed) */
  { "INTEL SSDPEKNW512G8", 512, 100,
    "Intel 660p spec sheet",
    "https://www.solidigm.com/content/dam/solidigm/en/site/products/client/d6/660p/documents/ssd-660p-series-1-0tb-m-2-80mm-pcie-3-0-x4-3d2-qlc-spec-sheet.pdf" },
  { "INTEL SSDPEKNW010T8", 1000, 200,
    "Intel 660p spec sheet",
    "https://www.solidigm.com/content/dam/solidigm/en/site/products/client/d6/660p/documents/ssd-660p-series-1-0tb-m-2-80mm-pcie-3-0-x4-3d2-qlc-spec-sheet.pdf" },
  { "INTEL SSDPEKNW020T8", 2000, 400,
    "Intel 660p spec sheet",
    "https://www.solidigm.com/content/dam/solidigm/en/site/products/client/d6/660p/documents/ssd-660p-series-1-0tb-m-2-80mm-pcie-3-0-x4-3d2-qlc-spec-sheet.pdf" },

  /* Intel 670p */
  { "INTEL SSDPEKNU512GZ", 512, 185,
    "Intel 670p product brief",
    "https://www.solidigm.com/content/dam/solidigm/en/site/products/client/d6/670p/documents/ssd-670p-series-product-brief.pdf" },
  { "INTEL SSDPEKNU010TZ", 1000, 370,
    "Intel 670p product brief",
    "https://www.solidigm.com/content/dam/solidigm/en/site/products/client/d6/670p/documents/ssd-670p-series-product-brief.pdf" },
  { "INTEL SSDPEKNU020TZ", 2000, 740,
    "Intel 670p product brief",
    "https://www.solidigm.com/content/dam/solidigm/en/site/products/client/d6/670p/documents/ssd-670p-series-product-brief.pdf" },

  /* Solidigm P41 Plus */
  { "SOLIDIGM SSDPFKNU512GZ", 512, 200,
    "Solidigm P41 Plus product brief",
    "https://www.solidigm.com/content/dam/solidigm/en/site/products/client/d6/p41/documents/P41-Plus-Product-Brief.pdf" },
  { "SOLIDIGM SSDPFKNU010TZ", 1000, 400,
    "Solidigm P41 Plus product brief",
    "https://www.solidigm.com/content/dam/solidigm/en/site/products/client/d6/p41/documents/P41-Plus-Product-Brief.pdf" },
  { "SOLIDIGM SSDPFKNU020TZ", 2000, 800,
    "Solidigm P41 Plus product brief",
    "https://www.solidigm.com/content/dam/solidigm/en/site/products/client/d6/p41/documents/P41-Plus-Product-Brief.pdf" },

  /* SK hynix Platinum P41, 2 TB only */
  { "SHPP41-2000GM", 2000, 1200,
    "SK hynix Platinum P41 product page",
    "https://ssd.skhynix.com/platinum_p41/" },
};


size_t
gsm_drive_spec_row_count (void)
{
  return G_N_ELEMENTS (drive_spec_rows);
}


const GsmDriveSpecRow *
gsm_drive_spec_row (size_t index)
{
  if (index >= G_N_ELEMENTS (drive_spec_rows))
    return NULL;

  return &drive_spec_rows[index];
}


static gboolean
model_starts_with_row (const char *model,
                       const char *prefix)
{
  gsize length = strlen (prefix);

  if (g_ascii_strncasecmp (model, prefix, length) != 0)
    return FALSE;

  return !g_ascii_isalnum (model[length]);
}


static gboolean
size_is_in_capacity_class (uint64_t size_bytes,
                           uint32_t capacity_gb)
{
  double nominal_bytes = (double) capacity_gb * 1e9;
  double tolerance_bytes = nominal_bytes * CAPACITY_TOLERANCE_PERCENT / 100.0;

  double difference_bytes = (double) size_bytes - nominal_bytes;

  if (difference_bytes < 0)
    difference_bytes = -difference_bytes;

  return difference_bytes <= tolerance_bytes;
}


const GsmDriveSpecRow *
gsm_drive_spec_lookup (const char *model,
                       uint64_t    size_bytes)
{
  if (model == NULL || *model == '\0' || size_bytes == 0)
    return NULL;

  for (gsize i = 0; i < G_N_ELEMENTS (drive_spec_rows); i++) {
    const GsmDriveSpecRow *row = &drive_spec_rows[i];

    if (!model_starts_with_row (model, row->model_prefix))
      continue;

    if (size_is_in_capacity_class (size_bytes, row->capacity_gb))
      return row;
  }

  return NULL;
}


uint64_t
gsm_drive_spec_rated_tbw (const char *model,
                          uint64_t    size)
{
  const GsmDriveSpecRow *row = gsm_drive_spec_lookup (model, size);

  return row != NULL ? (uint64_t) row->rated_tbw * GSM_TB : 0;
}


/*
 * A controller reports wear as a whole percent, so "12" means any true wear
 * from 12.00 to 12.99 %, and the endurance it implies (bytes written divided
 * by the fraction worn) lies between written / 0.13 and written / 0.12. Both
 * bounds are rounded down to whole bytes in integer arithmetic.
 */
static uint64_t
written_over_percent (uint64_t written_bytes,
                      uint64_t percent)
{
  return (written_bytes / percent) * 100 +
         (written_bytes % percent) * 100 / percent;
}


gboolean
gsm_drive_spec_implied_endurance (uint64_t           bytes_written,
                                  int                percent_used,
                                  GsmEnduranceRange *range)
{
  g_return_val_if_fail (range != NULL, FALSE);

  range->low_bytes = 0;
  range->high_bytes = 0;

  /* At 0 % the upper bound is unbounded; below zero the drive said nothing. */
  if (percent_used < 1 || bytes_written == 0)
    return FALSE;

  range->low_bytes = written_over_percent (bytes_written, (uint64_t) percent_used + 1);
  range->high_bytes = written_over_percent (bytes_written, (uint64_t) percent_used);

  return TRUE;
}


/*
 * Below this much wear the controller's percent_used is too coarse to divide
 * by: it is reported in whole percent, so at 1% the implied endurance carries
 * a ±50% error and at 2% still ±25%. Deriving from it would produce a
 * confident-looking number built on rounding noise.
 */
#define MIN_WEAR_TO_DERIVE 5


void
gsm_drive_resolve_endurance (GsmDrive *drive,
                             uint64_t  user_override)
{
  int percent_used;
  uint64_t written;
  const GsmDriveSpecRow *maker;
  GsmEnduranceRange implied = { 0, 0 };
  gboolean have_implied = FALSE;

  g_return_if_fail (GSM_IS_DRIVE (drive));

  percent_used = gsm_drive_get_percent_used (drive);
  written = gsm_drive_get_bytes_written (drive);

  /* Both truths are kept side by side whatever the projection follows: the
   * maker's figure for this exact model and capacity, if one is published,
   * and the range the drive's own wear implies, once it is worn enough. */
  maker = gsm_drive_spec_lookup (gsm_drive_get_model (drive),
                                 gsm_drive_get_size (drive));
  if (maker != NULL)
    gsm_drive_set_maker_rating (drive, (uint64_t) maker->rated_tbw * GSM_TB,
                                maker->source_label, maker->source_url);
  else
    gsm_drive_set_maker_rating (drive, 0, NULL, NULL);

  if (percent_used >= MIN_WEAR_TO_DERIVE)
    have_implied = gsm_drive_spec_implied_endurance (written, percent_used, &implied);
  gsm_drive_set_implied_endurance (drive, implied.low_bytes, implied.high_bytes);

  if (user_override > 0) {
    gsm_drive_set_endurance (drive, user_override, GSM_ENDURANCE_USER);
    return;
  }

  /* The single figure the projection uses is the upper bound of the range. */
  if (have_implied) {
    gsm_drive_set_endurance (drive, implied.high_bytes, GSM_ENDURANCE_DERIVED);
    return;
  }

  if (maker != NULL) {
    gsm_drive_set_endurance (drive, (uint64_t) maker->rated_tbw * GSM_TB,
                             GSM_ENDURANCE_SPEC_DB);
    return;
  }

  gsm_drive_set_endurance (drive, 0, GSM_ENDURANCE_UNKNOWN);
}


double
gsm_drive_project_years_left (GsmDrive *drive,
                              double    bytes_per_day)
{
  int percent_used;
  uint64_t rated;
  uint64_t written;
  double remaining_bytes;

  g_return_val_if_fail (GSM_IS_DRIVE (drive), -1);

  if (bytes_per_day <= 0)
    return -1;

  percent_used = gsm_drive_get_percent_used (drive);
  rated = gsm_drive_get_rated_tbw (drive);
  written = gsm_drive_get_bytes_written (drive);

  if (rated == 0)
    return -1;

  /*
   * Prefer the drive's own wear estimate to compute what is left. It already
   * accounts for write amplification and for wear the host never saw, so
   * "88% of the endurance remains" is a better basis than subtracting host
   * writes from a rating the drive may not agree with in the first place.
   */
  if (percent_used >= 0 && percent_used <= 100) {
    remaining_bytes = (double) rated * ((100.0 - percent_used) / 100.0);
  } else if (written < rated) {
    remaining_bytes = (double) (rated - written);
  } else {
    return 0;
  }

  if (remaining_bytes <= 0)
    return 0;

  return remaining_bytes / bytes_per_day / 365.25;
}
