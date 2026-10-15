// 従来形式の PiPL。hikariuni.vcxproj のカスタムビルドで cl /EP → Cnvtpipl.exe により hikariuni.pipl に変換し、
// hikariuni.rc に埋め込む
#include <PIDefines.h>
#include <PIResDefines.h>

resource 'PiPL' (16000, "Hikariuni", purgeable)
{
	{
		Kind { Filter },
		Name { "Hikariuni" },
		Category { "Hikariuni" },
		Version { (latestFilterVersion << 16) | latestFilterSubVersion },
		CodeWin64X86 { "PluginMain" },

		SupportedModes
		{
			noBitmap, doesSupportGrayScale,
			noIndexedColor, doesSupportRGBColor,
			noCMYKColor, noHSLColor,
			noHSBColor, noMultichannel,
			noDuotone, noLABColor
		},

		EnableInfo { "in (PSHOP_ImageMode, RGBMode, GrayScaleMode, RGB48Mode, Gray16Mode)" },

		PlugInMaxSize { 2000000, 2000000 },

		FilterCaseInfo
		{
			{
				/* Flat data, no selection */
				inStraightData, outStraightData,
				doNotWriteOutsideSelection,
				filtersLayerMasks, worksWithBlankData,
				copySourceToDestination,

				/* Flat data with selection */
				inStraightData, outStraightData,
				writeOutsideSelection,
				filtersLayerMasks, worksWithBlankData,
				copySourceToDestination,

				/* Floating selection */
				inStraightData, outStraightData,
				writeOutsideSelection,
				filtersLayerMasks, worksWithBlankData,
				copySourceToDestination,

				/* Editable transparency, no selection */
				inStraightData, outStraightData,
				doNotWriteOutsideSelection,
				filtersLayerMasks, worksWithBlankData,
				copySourceToDestination,

				/* Editable transparency, with selection */
				inStraightData, outStraightData,
				writeOutsideSelection,
				filtersLayerMasks, worksWithBlankData,
				copySourceToDestination,

				/* Preserved transparency, no selection */
				inStraightData, outStraightData,
				doNotWriteOutsideSelection,
				filtersLayerMasks, worksWithBlankData,
				copySourceToDestination,

				/* Preserved transparency, with selection */
				inStraightData, outStraightData,
				writeOutsideSelection,
				filtersLayerMasks, worksWithBlankData,
				copySourceToDestination
			}
		}
	}
};
