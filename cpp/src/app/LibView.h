// Library (app/js/ui/lib.js): the built-in 808 kit and stock instruments, audio imported into the project, and sample
// folders on disk (Settings › File), each in a card. Click a name to audition; "+ Rack" adds a channel, "→ Selected" puts
// it on the selected sampler; drag any sound onto the Rack or a Song track. Folder files are only imported when used.
#pragma once
#include "Cards.h"
#include "Doc.h"
#include "Theme.h"

namespace ui {

class LibView : public juce::Component, private juce::ChangeListener {
public:
    explicit LibView(Doc& d) : doc(d) {
        doc.addChangeListener(this); addAndMakeVisible(cv);
        for (juce::Component* c : std::initializer_list<juce::Component*>{ &uploadB, &folderB, &find, &hint }) cv.addAndMakeVisible(c);
        cv.makeBar = [this](Flow& f) { f.add(uploadB, uploadB.idealWidth()).add(folderB, folderB.idealWidth()).add(find, 180).add(hint, hint.idealWidth()); };
        uploadB.primary = true; uploadB.onClick = [this] { importDialog(); };
        folderB.setTooltip("Browse a folder of samples without importing everything"); folderB.onClick = [this] { addFolder(); };
        find.setTextToShowWhenEmpty(u8("Filter folder files…"), DIM); find.onTextChange = [this] { build(); };
        cv.grid.minCol = 280; build();
    }
    ~LibView() override { doc.removeChangeListener(this); }
    void resized() override { cv.setBounds(getLocalBounds()); }

private:
    Doc& doc; CardView cv; Btn uploadB{ u8("Upload audio…") }, folderB{ "+ Sample folder" }; Field find; Cap hint{ "Tap a name to audition - drag any sound onto the Rack or a Song track" };
    std::unique_ptr<juce::FileChooser> chooser; juce::String sig;
    // one row: the name (click to hear, drag to use), + Rack, → Selected, and ✕ for your own sounds
    struct Row : juce::Component {
        LibView* o = nullptr; std::string id; juce::File file; juce::String name, sub; Btn rack{ "+ Rack" }, sel{ u8("→ Selected") }, del{ u8("✕") }; bool own = false;
        Row() { for (auto* b : { &rack, &sel, &del }) { b->sm = true; addAndMakeVisible(b); } rack.setTooltip("Add as a new channel"); sel.setTooltip("Use as the sound of the selected sampler channel"); del.setTooltip("Remove from library"); }
        void resized() override { auto r = getLocalBounds(); if (own) { del.setBounds(r.removeFromRight(30)); r.removeFromRight(5); } del.setVisible(own); sel.setBounds(r.removeFromRight(sel.idealWidth())); r.removeFromRight(5); rack.setBounds(r.removeFromRight(rack.idealWidth())); r.removeFromRight(5); nameR = r; }
        juce::Rectangle<int> nameR;
        void paint(juce::Graphics& g) override { auto r = nameR.toFloat(); g.setColour(isMouseOver() && nameR.contains(getMouseXYRelative()) ? mix(ACCENT, BG3, 0.22f) : BG3); g.fillRoundedRectangle(r, RAD() * 0.6f);
            g.setColour(TEXT); g.setFont(font(FS)); g.drawText(name, nameR.reduced(9, 0).withTrimmedRight(textW(font(FS * 0.88f), sub) + 8), juce::Justification::centredLeft, true); g.setColour(DIM); g.setFont(font(FS * 0.88f)); g.drawText(sub, nameR.reduced(9, 0), juce::Justification::centredRight, true); }
        void mouseMove(const juce::MouseEvent&) override { repaint(); } void mouseExit(const juce::MouseEvent&) override { repaint(); }
        void mouseUp(const juce::MouseEvent& e) override { if (nameR.contains(e.getPosition()) && !e.mouseWasDraggedSinceMouseDown()) o->audition(*this); }
        void mouseDrag(const juce::MouseEvent& e) override { if (e.getDistanceFromDragStart() < 6 || !nameR.contains(e.getMouseDownPosition())) return; if (auto* dc = juce::DragAndDropContainer::findParentDragContainerFor(this)) if (!dc->isDragAndDropActive()) { const auto sid = id.empty() ? o->importFile(file) : id; if (!sid.empty()) dc->startDragging("sample:" + juce::String(sid), this); } }
    };
    std::string importFile(const juce::File& f) { auto id = doc.importAudio(f); if (id.empty()) toast("Could not decode " + f.getFileName()); else { doc.dirty = true; } return id; }
    void audition(Row& r) {
        if (!r.id.empty()) { doc.eng.previewSample(r.id); return; }
        // a folder file: decoded into a temporary preview sound, not imported
        juce::MemoryBlock mb; if (!r.file.loadFileAsData(mb)) return; if (doc.bank.add("lib:preview", r.file.getFileNameWithoutExtension().toStdString(), mb)) doc.eng.previewSample("lib:preview"); else toast("Could not play " + r.file.getFileName());
    }
    void toRack(const std::string& id) {
        auto s = doc.bank.get(id); if (!s) return; const bool drum = id.rfind("808:", 0) == 0;
        juce::String nm = juce::String(s->name).upToFirstOccurrenceOf(" (", false, false).substring(0, 18);
        auto c = newChannel(doc.p, "sample", nm.toStdString(), id); c.gate = !drum && s->seconds() > 1.2; c.rel = 0.25; c.root = id == "inst:sub" ? 36 : 60; doc.p.channels.push_back(c); doc.ch = c.id; doc.commit(); toast(u8("Added “") + nm + u8("” to the rack"));
    }
    void toSel(const std::string& id) { auto* c = doc.chan(); if (!c || c->type != "sample") { toast("Select a sampler channel first"); return; } c->sample = id; doc.commit(); if (auto s = doc.bank.get(id)) toast(u8("“") + juce::String(c->name) + u8("” now plays ") + juce::String(s->name)); }
    Row* row(const std::string& id, const juce::String& name, const juce::String& sub, bool own) {
        auto* r = cv.grid.full(new Row(), CTL()); r->o = this; r->id = id; r->name = name; r->sub = sub; r->own = own;
        r->rack.onClick = [this, id] { toRack(id); }; r->sel.onClick = [this, id] { toSel(id); };
        r->del.onClick = [this, id] { doc.p.embedded->data.erase(id); doc.p.embedded->names.erase(id); doc.dirty = true; toast("Removed from this project (it stays until the app restarts)"); doc.sendChangeMessage(); };
        return r;
    }
    juce::StringArray folders() const { auto l = juce::StringArray::fromLines(doc.ps("sampleFolders", "")); l.removeEmptyStrings(); return l; }
    void build() {
        auto& G = cv.grid; G.clear(); const auto q = find.getText().trim().toLowerCase();
        for (auto& path : folders()) {
            juce::File dir(path); auto files = dir.findChildFiles(juce::File::findFiles, true, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3"); std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.getFullPathName() < b.getFullPathName(); });
            juce::Array<juce::File> hits; for (auto& f : files) if (q.isEmpty() || f.getRelativePathFrom(dir).toLowerCase().contains(q)) hits.add(f);
            G.card(u8("📁 ") + dir.getFileName());
            auto* x = new Btn(u8("✕")); x->sm = true; x->setTooltip("Forget this folder"); x->onClick = [this, path] { auto l = folders(); l.removeString(path); doc.setp("sampleFolders", l.joinIntoString("\n")); build(); }; G.titleRight(x, 30);
            G.note(dir.isDirectory() ? juce::String(hits.size()) + " of " + juce::String(files.size()) + " files" : juce::String("Folder not found"));
            for (int i = 0; i < std::min(250, hits.size()); i++) { auto f = hits[i]; auto rel = f.getParentDirectory().getRelativePathFrom(dir); auto* r = row({}, f.getFileNameWithoutExtension(), rel == "." ? juce::String() : rel, false); r->file = f;
                r->rack.setTooltip("Import and add as a new channel"); r->sel.setTooltip("Import and use for the selected sampler channel");
                r->rack.onClick = [this, f] { auto id = importFile(f); if (!id.empty()) toRack(id); }; r->sel.onClick = [this, f] { auto id = importFile(f); if (!id.empty()) toSel(id); }; }
            if (hits.size() > 250) G.note("Showing the first 250 - type in the filter box to narrow " + juce::String(hits.size()) + " files.");
            if (hits.isEmpty()) G.note(q.isNotEmpty() ? "Nothing matches the filter." : "No audio files found in this folder.");
        }
        std::map<std::string, std::vector<us::SampleRef>> groups; std::vector<std::string> order;
        for (auto& s : doc.bank.kit()) { if (!groups.count(s->group)) order.push_back(s->group); groups[s->group].push_back(s); }
        for (auto& g : order) { G.card(juce::String(g)); for (auto& s : groups[g]) row(s->id, juce::String(s->name), juce::String(s->seconds(), 2) + "s", false); }
        G.card("Your audio"); bool any = false;
        for (auto& s : doc.bank.imported()) { if (!doc.p.embedded->data.count(s->id)) continue; any = true; row(s->id, juce::String(s->name), juce::String(s->seconds(), 2) + "s", true); }
        if (!any) G.note("Nothing here yet. Upload any audio file to use it as a drum hit or a playable instrument.");
        cv.resized();
    }
    void importDialog() {
        chooser = std::make_unique<juce::FileChooser>("Upload audio", juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems, [this](const juce::FileChooser& fc) {
            for (auto& f : fc.getResults()) importFile(f); doc.dirty = true; doc.sendChangeMessage(); });
    }
    void addFolder() {
        chooser = std::make_unique<juce::FileChooser>("Sample folder", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) { auto d = fc.getResult(); if (!d.isDirectory()) return; auto l = folders(); l.addIfNotAlreadyThere(d.getFullPathName()); doc.setp("sampleFolders", l.joinIntoString("\n")); build(); });
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        juce::String s; for (auto& x : doc.bank.imported()) s << x->id; for (auto& [k, v] : doc.p.embedded->data) s << k; s << doc.ps("sampleFolders", "");
        if (s != sig) { sig = s; juce::MessageManager::callAsync([sp = juce::Component::SafePointer<LibView>(this)] { if (sp) sp->build(); }); }
    }
};

}   // namespace ui
