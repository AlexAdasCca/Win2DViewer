#pragma once

class CSvgDocument
{
  public:
    bool LoadFromFile(std::wstring_view path, std::wstring* errorMessage = nullptr);
    void Clear() noexcept;

    bool Empty() const noexcept
    {
        return svgXmlBytes.empty();
    }
    const std::vector<char>& GetSvgXml() const noexcept
    {
        return svgXmlBytes;
    }
    std::wstring_view GetPath() const noexcept
    {
        return documentPath;
    }

  private:
    std::vector<char> svgXmlBytes;
    std::wstring documentPath;
};
