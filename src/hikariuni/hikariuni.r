// 従来形式の PiPL。hikariuni.vcxproj のカスタムビルドで cl /EP → Cnvtpipl.exe により hikariuni.pipl に変換し、
// hikariuni.rc に埋め込む
#include <PIDefines.h>
#include <PIResDefines.h>

resource 'PiPL' (16000, "Hikariuni", purgeable)
{
	{
		Kind { Filter },
		Name { "Hikariuni..." },
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

		// 7 つのケース（平面・選択範囲・フローティング・透明度の編集可／保護 × 選択範囲の有無）はすべて同じ扱い
		#define HIKARIUNI_CASE inStraightData, outStraightData, doNotWriteOutsideSelection, \
			filtersLayerMasks, worksWithBlankData, copySourceToDestination
		FilterCaseInfo
		{
			{
				HIKARIUNI_CASE, HIKARIUNI_CASE, HIKARIUNI_CASE, HIKARIUNI_CASE,
				HIKARIUNI_CASE, HIKARIUNI_CASE, HIKARIUNI_CASE
			}
		}
	}
};
