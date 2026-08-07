// Version.h — 제품 식별 정보의 단일 진실 공급원.
#pragma once

#define IMI_PRODUCT_NAME_A   "InputMethodIndicator"
#define IMI_PRODUCT_NAME_W   L"InputMethodIndicator"

#define IMI_COPYRIGHT_A      "Copyright (c) 2026 elsewon. MIT License."

// 소스 저장소. 이상 제보 시 GitHub 새 이슈 URL의 기준이 된다.
#define IMI_REPO_URL_W       L"https://github.com/elsewon/InputMethodIndicator"

#define IMI_VERSION_MAJOR    0
#define IMI_VERSION_MINOR    1
#define IMI_VERSION_PATCH    0
#define IMI_VERSION_BUILD    0

#define IMI_STR2(x) #x
#define IMI_STR(x)  IMI_STR2(x)

#define IMI_VERSION_STRING_A \
    IMI_STR(IMI_VERSION_MAJOR) "." IMI_STR(IMI_VERSION_MINOR) "." IMI_STR(IMI_VERSION_PATCH)
#define IMI_VERSION_STRING_W \
    L"" IMI_VERSION_STRING_A
