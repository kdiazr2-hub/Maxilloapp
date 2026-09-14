#pragma once

#include <QString>
#include <QStringList>

enum class ValidationSeverity
{
    Ok,
    Warning,
    Error
};

struct ValidationResult
{
    ValidationSeverity severity = ValidationSeverity::Ok;
    QString message;
    QStringList details;

    bool ok() const { return severity == ValidationSeverity::Ok; }
    bool warning() const { return severity == ValidationSeverity::Warning; }
    bool isWarn() const { return warning(); }
    bool isError() const { return severity == ValidationSeverity::Error; }
};
