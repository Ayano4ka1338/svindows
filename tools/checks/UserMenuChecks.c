/* Exercise the actual USER menu implementation without a guest boot.
 * Build with clang, function sections and --gc-sections; unreferenced native
 * UI/loader entry points are discarded, leaving the menu model/parser. */
#include <stdio.h>
#include <stdlib.h>
#include "../../user/w/user32/User32.c"
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
int main(void)
{
    HMENU root=CreateMenu(),sub=CreatePopupMenu();
    CHECK(root&&sub&&!sv_menu((HMENU)1));
    CHECK(AppendMenuW(sub,0,100,L"&One"));
    CHECK(AppendMenuW(sub,0,101,L"&Two"));
    CHECK(AppendMenuW(root,MF_POP,(UINT_PTR)sub,L"&View"));
    CHECK(GetSubMenu(root,0)==sub&&GetMenuItemCount(root)==1);
    CHECK(GetMenuState(root,100,0)==0);
    CHECK(CheckMenuRadioItem(root,100,101,101,0));
    CHECK(!(GetMenuState(root,100,0)&8));CHECK(GetMenuState(root,101,0)&8);
    CHECK(EnableMenuItem(root,100,3)==0);CHECK((GetMenuState(root,100,0)&3)==3);
    CHECK(EnableMenuItem(root,100,0)==3);
    SV_MENU_INFO info={0};info.cbSize=sizeof(info);info.fMask=0x42;info.wID=102;info.dwTypeData=L"Inserted";
    CHECK(InsertMenuItemW(sub,1,TRUE,&info));CHECK(GetMenuItemID(sub,1)==102);
    info.fMask=4;info.hSubMenu=(HMENU)1;
    CHECK(!InsertMenuItemW(sub,0,TRUE,&info));CHECK(GetMenuItemCount(sub)==3);
    CHECK(!AppendMenuW(sub,MF_POP,1,L"bad"));CHECK(GetMenuItemCount(sub)==3);
    CHECK(RemoveMenu(root,102,0));CHECK(GetMenuItemCount(sub)==2);
    CHECK(RemoveMenu(root,0,MF_BY_POS));CHECK(GetMenuItemCount(root)==0);
    CHECK(GetMenuItemCount(sub)==2);CHECK(DestroyMenu(sub));CHECK(GetMenuItemCount(sub)==-1);
    CHECK(DestroyMenu(root));CHECK(!DestroyMenu(root));
    /* Popup has no command WORD; its children terminate at their own END. */
    static const WORD resource[]={0x10,'V',0,0,200,'A',0,0,0,0,0x80,201,'B',0,0x80,300,'E',0};
    root=CreateMenu();const unsigned char *cursor=(const unsigned char*)resource;
    CHECK(sv_menu_parse(root,&cursor,(const unsigned char*)(resource+sizeof(resource)/2),0));
    CHECK(GetMenuItemCount(root)==2);sub=GetSubMenu(root,0);CHECK(sub);
    CHECK(GetMenuItemCount(sub)==3);CHECK(GetMenuState(sub,1,MF_BY_POS)&MF_SEPARATOR_);
    CHECK(GetMenuItemID(root,1)==300);
    CHECK(DestroyMenu(root));CHECK(GetMenuItemCount(sub)==-1);
    root=CreateMenu();cursor=(const unsigned char*)resource;
    CHECK(!sv_menu_parse(root,&cursor,(const unsigned char*)resource+3,0));CHECK(DestroyMenu(root));
    /* Extended menu with one popup, a help ID, and a checked command. */
    static const DWORD extended[] = {
        0, 0, 99, 0x00560081, 0, 123,
        0, 8, 401, 0x00410080, 0
    };
    root = CreateMenu();
    cursor = (const unsigned char *)extended;
    CHECK(sv_menu_parse_ex(root, &cursor, (const unsigned char *)(extended + 11), 0));
    sub = GetSubMenu(root, 0);
    CHECK(sub && GetMenuItemCount(sub) == 1);
    CHECK(GetMenuItemID(sub, 0) == 401 && (GetMenuState(sub, 401, 0) & 8));
    CHECK(DestroyMenu(root));
    root = CreateMenu();
    cursor = (const unsigned char *)extended;
    CHECK(!sv_menu_parse_ex(root, &cursor, (const unsigned char *)extended + 22, 0));
    CHECK(DestroyMenu(root));
    SV_USER_WINDOW_STATE *old=sv_user_state((HWND)0x10011);CHECK(old);
    old->Longs[0]=42;old->Menu=CreateMenu();HMENU stale=old->Menu;
    SV_USER_WINDOW_STATE *fresh=sv_user_state((HWND)0x20011);CHECK(fresh&&fresh->Longs[0]==0);
    CHECK(!sv_menu(stale));
    puts("USER menus: nested resources, state, radio groups, insertion, removal, invalid handles and truncation passed");
    return 0;
}
