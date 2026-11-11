#include "HomePanel.hpp"

#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/statbmp.h>
#include <wx/filename.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "MsgDialog.hpp"
#include "GUI.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI {

namespace {
bool is_dark() { return wxGetApp().dark_mode(); }
wxColour page_bg() { return is_dark() ? wxColour("#2D2D31") : wxColour("#F7F7F7"); }
wxColour tile_bg() { return is_dark() ? wxColour("#3B3B40") : *wxWHITE; }
wxColour tile_border() { return is_dark() ? wxColour("#4A4A51") : wxColour("#DBDBDB"); }
wxColour text_fg() { return is_dark() ? wxColour("#EFEFF0") : wxColour("#262E30"); }
wxColour sub_fg() { return is_dark() ? wxColour("#A0A0A6") : wxColour("#6B6B6B"); }
wxColour thumb_bg() { return is_dark() ? wxColour("#33333A") : wxColour("#EEEEEE"); }
} // namespace

HomePanel::HomePanel(wxWindow *parent) : wxPanel(parent, wxID_ANY)
{
    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    build_header();

    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scroll->SetScrollRate(0, FromDIP(20));
    m_grid = new wxWrapSizer(wxHORIZONTAL);
    auto *scroll_sizer = new wxBoxSizer(wxVERTICAL);
    m_empty = new Label(m_scroll, Label::Body_14, _L("No recent projects yet. Open or create a project to get started."));
    scroll_sizer->Add(m_empty, 0, wxALL, FromDIP(12));
    scroll_sizer->Add(m_grid, 1, wxEXPAND | wxALL, FromDIP(4));
    m_scroll->SetSizer(scroll_sizer);
    m_main_sizer->Add(m_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));

    SetSizer(m_main_sizer);
    apply_colours();
    rebuild();
}

void HomePanel::build_header()
{
    const bool dark = is_dark();
    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(144, 144, 144), StateColor::Disabled),
                            std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered),
                            std::pair<wxColour, int>(dark ? wxColour("#3A8F44") : wxColour("#21A452"), StateColor::Normal));
    StateColor btn_text_white(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal));
    StateColor btn_bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                            std::pair<wxColour, int>(dark ? wxColour("#3B3B40") : *wxWHITE, StateColor::Normal));
    StateColor btn_bd_white(std::pair<wxColour, int>(dark ? wxColour("#5A5A61") : wxColour(38, 46, 48), StateColor::Normal));
    StateColor btn_text_dark(std::pair<wxColour, int>(text_fg(), StateColor::Normal));

    auto make_btn = [&](const wxString &label, bool primary) {
        auto *b = new Button(this, label);
        b->SetBackgroundColor(primary ? btn_bg_green : btn_bg_white);
        b->SetBorderColor(primary ? btn_bg_green : btn_bd_white);
        b->SetTextColor(primary ? btn_text_white : btn_text_dark);
        b->SetFont(Label::Body_14);
        b->SetCornerRadius(FromDIP(14));
        b->SetMinSize(wxSize(FromDIP(130), FromDIP(32)));
        return b;
    };

    auto *top = new wxBoxSizer(wxHORIZONTAL);
    m_btn_new  = make_btn(_L("New Project"), true);
    m_btn_open = make_btn(_L("Open Project"), false);
    m_btn_new->Bind(wxEVT_BUTTON, [](wxCommandEvent &) { wxGetApp().request_open_project("<new>"); });
    m_btn_open->Bind(wxEVT_BUTTON, [](wxCommandEvent &) { wxGetApp().request_open_project({}); });
    top->Add(m_btn_new, 0, wxRIGHT, FromDIP(12));
    top->Add(m_btn_open, 0);
    m_main_sizer->Add(top, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(24));

    auto *title_row = new wxBoxSizer(wxHORIZONTAL);
    m_title = new Label(this, Label::Head_16, _L("Recent projects"));
    title_row->Add(m_title, 0, wxALIGN_CENTER_VERTICAL);
    title_row->AddStretchSpacer(1);
    m_btn_clear = make_btn(_L("Clear list"), false);
    m_btn_clear->SetFont(Label::Body_12);
    m_btn_clear->SetMinSize(wxSize(FromDIP(90), FromDIP(26)));
    m_btn_clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        MessageDialog dlg(this, _L("Remove all projects from the recent list? The files are not deleted."), _L("Clear recent projects"), wxYES_NO | wxNO_DEFAULT);
        if (dlg.ShowModal() == wxID_YES)
            wxGetApp().request_remove_project("");
    });
    title_row->Add(m_btn_clear, 0, wxALIGN_CENTER_VERTICAL);
    m_main_sizer->Add(title_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(24));
    m_main_sizer->AddSpacer(FromDIP(10));
}

void HomePanel::apply_colours()
{
    SetBackgroundColour(page_bg());
    m_scroll->SetBackgroundColour(page_bg());
    m_title->SetForegroundColour(text_fg());
    m_title->SetBackgroundColour(page_bg());
    m_empty->SetForegroundColour(sub_fg());
    m_empty->SetBackgroundColour(page_bg());
}

void HomePanel::SendRecentList(int)
{
    // Called from event handlers that may live on a tile we are about to destroy.
    if (m_rebuild_pending) return;
    m_rebuild_pending = true;
    CallAfter([this] {
        m_rebuild_pending = false;
        rebuild();
    });
}

void HomePanel::rebuild()
{
    MainFrame *mf = wxGetApp().mainframe;
    m_scroll->Freeze();
    m_grid->Clear(true);

    std::vector<MainFrame::RecentProjectInfo> items;
    if (mf) items = mf->get_recent_project_list();

    for (size_t i = 0; i < items.size(); ++i) {
        const auto &it = items[i];
        wxWindow *tile = make_tile(m_scroll, it.index, it.path, it.name, it.time, it.missing, it.thumbnail_png);
        m_grid->Add(tile, 0, wxALL, FromDIP(8));
    }
    m_empty->Show(items.empty());
    m_btn_clear->Show(!items.empty());

    m_scroll->Layout();
    m_scroll->FitInside();
    m_scroll->Thaw();
    Layout();
}

wxWindow *HomePanel::make_tile(wxWindow *parent, size_t index, const wxString &path, const wxString &name, const wxString &time, bool missing, const std::string &png)
{
    const int thumb_w = FromDIP(200), thumb_h = FromDIP(150);
    auto *tile = new wxPanel(parent, wxID_ANY);
    tile->SetBackgroundColour(tile_bg());
    tile->SetToolTip(path);
    auto *sizer = new wxBoxSizer(wxVERTICAL);

    // Thumbnail (scaled to fit, centred on a neutral canvas), or a placeholder.
    wxBitmap canvas(thumb_w, thumb_h);
    {
        wxMemoryDC dc(canvas);
        dc.SetBackground(wxBrush(thumb_bg()));
        dc.Clear();
        wxImage img;
        if (!png.empty()) {
            wxMemoryInputStream stream(png.data(), png.size());
            img.LoadFile(stream, wxBITMAP_TYPE_PNG);
        }
        if (img.IsOk() && img.GetWidth() > 0 && img.GetHeight() > 0) {
            double s = std::min(double(thumb_w) / img.GetWidth(), double(thumb_h) / img.GetHeight());
            int w = std::max(1, int(img.GetWidth() * s)), h = std::max(1, int(img.GetHeight() * s));
            img.Rescale(w, h, wxIMAGE_QUALITY_HIGH);
            dc.DrawBitmap(wxBitmap(img), (thumb_w - w) / 2, (thumb_h - h) / 2, true);
        } else {
            dc.SetTextForeground(sub_fg());
            dc.SetFont(Label::Body_13);
            wxString t = missing ? _L("File is missing") : _L("No preview");
            wxSize ts = dc.GetTextExtent(t);
            dc.DrawText(t, (thumb_w - ts.x) / 2, (thumb_h - ts.y) / 2);
        }
        dc.SelectObject(wxNullBitmap);
    }
    auto *bmp = new wxStaticBitmap(tile, wxID_ANY, canvas);
    sizer->Add(bmp, 0, wxALL, FromDIP(8));

    auto *name_lbl = new Label(tile, Label::Body_14, name, wxST_ELLIPSIZE_END);
    name_lbl->SetForegroundColour(text_fg());
    name_lbl->SetBackgroundColour(tile_bg());
    name_lbl->SetMinSize(wxSize(thumb_w, -1));
    name_lbl->SetMaxSize(wxSize(thumb_w, -1));
    sizer->Add(name_lbl, 0, wxLEFT | wxRIGHT, FromDIP(8));

    auto *time_lbl = new Label(tile, Label::Body_12, time);
    time_lbl->SetForegroundColour(sub_fg());
    time_lbl->SetBackgroundColour(tile_bg());
    sizer->Add(time_lbl, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, FromDIP(8));
    tile->SetSizer(sizer);

    // Border painted around the tile.
    tile->Bind(wxEVT_PAINT, [tile](wxPaintEvent &) {
        wxPaintDC dc(tile);
        dc.SetPen(wxPen(tile_border(), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        wxSize sz = tile->GetClientSize();
        dc.DrawRectangle(0, 0, sz.x, sz.y);
    });

    auto open = [index, path](wxMouseEvent &) {
        if (MainFrame *mf = wxGetApp().mainframe)
            mf->open_recent_project(index, path);
    };
    auto menu = [this, index, path](wxMouseEvent &) { show_tile_menu(index, path); };
    for (wxWindow *w : std::initializer_list<wxWindow *>{tile, bmp, name_lbl, time_lbl}) {
        w->SetCursor(wxCursor(wxCURSOR_HAND));
        w->Bind(wxEVT_LEFT_UP, open);
        w->Bind(wxEVT_RIGHT_UP, menu);
        if (w != tile) w->SetToolTip(path);
    }
    return tile;
}

void HomePanel::show_tile_menu(size_t index, const wxString &path)
{
    wxMenu menu;
    const int id_open = wxNewId(), id_folder = wxNewId(), id_remove = wxNewId();
    menu.Append(id_open, _L("Open"));
    menu.Append(id_folder, _L("Show in folder"));
    menu.Append(id_remove, _L("Remove from list"));
    const int sel = GetPopupMenuSelectionFromUser(menu);
    if (sel == id_open) {
        if (MainFrame *mf = wxGetApp().mainframe) mf->open_recent_project(index, path);
    } else if (sel == id_folder) {
        if (wxFileExists(path))
            desktop_open_any_folder(into_u8(path));
        else
            desktop_open_any_folder(into_u8(wxFileName(path).GetPath()));
    } else if (sel == id_remove) {
        wxGetApp().request_remove_project(into_u8(path));
    }
}

void HomePanel::msw_rescale()
{
    m_btn_new->Rescale();
    m_btn_open->Rescale();
    m_btn_clear->Rescale();
    rebuild();
}

void HomePanel::sys_color_changed()
{
    apply_colours();
    rebuild();
    Refresh();
}

}} // namespace Slic3r::GUI
