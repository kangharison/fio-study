/*
 * [한국어 설명] Windows 타이머 해상도 조회 (windows/dlls.c)
 *
 * === 파일의 역할 ===
 * Windows에서 시스템 타이머 해상도(clock tick)를 조회하는 os_clk_tck() 구현.
 * ntdll.dll의 NtQueryTimerResolution/NtSetTimerResolution을 사용하여
 * 최대 해상도로 설정하고, 초당 틱 수를 반환.
 *
 * === 전체 아키텍처에서의 위치 ===
 * os/os.h에서 _SC_CLK_TCK 미정의 시 os_clk_tck() 외부 함수 선언 → 이 파일 구현.
 * gettime.c, stat.c 등에서 CPU 시간 계산에 사용.
 *
 * === 주요 함수 요약 ===
 * - os_clk_tck(): ntdll에서 최대 해상도 조회, 실패 시 기본값 64Hz
 */
#include "os/os.h"

#include <windows.h>

/*
 * [한국어]
 * os_clk_tck - Windows 타이머 해상도(clk_tck) 조회
 * @clk_tck: 초당 틱 수 저장 포인터
 *
 * ntdll.dll에서 NtQueryTimerResolution으로 해상도를 조회하고
 * NtSetTimerResolution으로 최대 해상도를 설정한 후 변환.
 * ntdll 로드 실패 시 하한값 64Hz 사용.
 */
void os_clk_tck(long *clk_tck)
{
	/*
	 * The timer resolution is variable on Windows. Try to query it 
	 * or use 64 Hz, the clock frequency lower bound. See also
	 * https://carpediemsystems.co.uk/2019/07/18/windows-system-timer-granularity/.
	 */
	unsigned long minRes, maxRes, curRes;
	HMODULE lib;
	NTSTATUS NTAPI (*queryTimer)
		(OUT PULONG              MinimumResolution,
		 OUT PULONG              MaximumResolution,
		 OUT PULONG              CurrentResolution);
	NTSTATUS NTAPI (*setTimer)
		(IN ULONG                DesiredResolution,
		 IN BOOLEAN              SetResolution,
		 OUT PULONG              CurrentResolution);

	if (!(lib = LoadLibrary(TEXT("ntdll.dll"))) ||
		!(queryTimer = (void *)GetProcAddress(lib, "NtQueryTimerResolution")) ||
		!(setTimer = (void *)GetProcAddress(lib, "NtSetTimerResolution"))) {
		dprint(FD_HELPERTHREAD, 
			"Failed to load ntdll library, set to lower bound 64 Hz\n");
		*clk_tck = 64;
	} else {
		queryTimer(&minRes, &maxRes, &curRes);
		dprint(FD_HELPERTHREAD, 
			"minRes = %lu, maxRes = %lu, curRes = %lu\n",
			minRes, maxRes, curRes);

		/* Use maximum resolution for most accurate timestamps */
		setTimer(maxRes, 1, &curRes);
		*clk_tck = (long) (10000000L / maxRes);
	}
}
