#pragma once

#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"

struct DropDownComponent : public juce::Component
{

    /* investigate using this later, thanks to Ryan Robinson in AudioProgrammer Discord
    struct Group;
    struct Item;
    using Node = std::variant<Group, Item>;

    struct Group
    {
        std::string name{};
        std::vector<Node> children{};
    };

    struct Item
    {
        std::string name{};
        int64_t id = -1;
    };
    */
    struct Node
    {
        std::string text;
        int64_t id = -1;
        std::vector<Node> children;
    };
    Node rootNode;
    DropDownComponent() {}
    bool buildMenusRecur(Node &n, juce::PopupMenu &menu)
    {
        if (n.children.size() == 0)
        {
            bool ticked = selectedId == n.id;
            menu.addItem(n.text, true, ticked, [this, n]() {
                selectedId = n.id;
                if (OnItemSelected)
                    OnItemSelected();
                selectedText = n.text;
                repaint();
            });
            return ticked;
        }
        else
        {
            juce::PopupMenu submenu;
            bool anyTicked = false;
            for (auto &e : n.children)
                anyTicked |= buildMenusRecur(e, submenu);

            menu.addSubMenu(n.text, submenu, true, nullptr, anyTicked);
            return anyTicked;
        }
    }
    Node *findNodeRecur(Node &n, int64_t tofind)
    {
        if (n.id == tofind)
            return &n;
        for (int i = 0; i < n.children.size(); ++i)
        {
            auto ptr = findNodeRecur(n.children[i], tofind);
            if (ptr)
                return ptr;
        }
        return nullptr;
    }
    void setSelectedId(int64_t id)
    {
        Node *found = nullptr;
        for (auto &e : rootNode.children)
        {
            found = findNodeRecur(e, id);
            if (found)
                break;
        }
        if (found)
        {
            selectedId = found->id;
            selectedText = found->text;
            repaint();
        }
    }

    void showNodes()
    {
        juce::PopupMenu menu;
        if (!rootNode.text.empty())
        {
            menu.addSectionHeader(rootNode.text);
            menu.addSeparator();
        }
        for (auto &e : rootNode.children)
        {
            buildMenusRecur(e, menu);
        }
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this));
    }

    int getSelectedId() { return selectedId; }
    juce::Font myfont;
    void paint(juce::Graphics &g) override
    {
        g.fillAll(juce::Colours::black);
        g.setFont(myfont);
        g.setColour(juce::Colours::white);
        g.drawRect(0, 0, getWidth(), getHeight(), 2);
        g.drawText(selectedText, 4, 2, getWidth(), getHeight() - 4,
                   juce::Justification::centredLeft);
    }
    void mouseDown(const juce::MouseEvent &ev) override { showMenu(); }
    void showMenu() { showNodes(); }

    int64_t selectedId = 0;
    std::string selectedText;
    std::function<void(void)> OnItemSelected;
};

class GalleryPicker : public juce::Component
{
  public:
    struct Item
    {
        int64_t id = 0;
        std::string text;
        juce::Rectangle<float> rect;
    };
    struct Category
    {
        std::string text;
        juce::Rectangle<float> rect;
        std::vector<Item> items;
    };
    std::vector<Category> categories;
    std::function<void(int64_t)> OnSelected;
    int64_t selectedID = -1;
    float cellw = 145.0f;
    GalleryPicker() { setWantsKeyboardFocus(true); }
    std::optional<std::string> get_text_from_id(int64_t id)
    {
        for (auto &c : categories)
        {
            for (auto &it : c.items)
            {
                if (it.id == id)
                {
                    return it.text;
                }
            }
        }
        return {};
    }
    bool keyPressed(const juce::KeyPress &ev) override
    {
        if (ev.getKeyCode() == juce::KeyPress::escapeKey)
        {
            setVisible(false);
            return true;
        }
        return false;
    }
    void resized() override;
    void mouseDown(const juce::MouseEvent &ev) override
    {
        int64_t id = -1;
        for (auto &cate : categories)
        {
            for (auto &it : cate.items)
            {
                if (it.rect.contains(ev.position))
                {
                    id = it.id;
                    break;
                }
            }
            if (id != -1)
                break;
        }
        if (id != -1 && OnSelected)
        {
            selectedID = id;
            OnSelected(id);
            repaint();
        }
    }
    void paint(juce::Graphics &g) override;
};